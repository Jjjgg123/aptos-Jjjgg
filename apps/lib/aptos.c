#include <stdarg.h>
#include "aptos.h"

/* ---------- heap: malloc/free/calloc/realloc over sys_brk ---------- */
typedef struct mhdr {
    struct mhdr *next;
    unsigned    size;   /* 用户可用字节 */
    int         free;
    const char *ac;     /* 分配者类（dbg 快照） */
    const char *am;     /* 分配者方法 */
    unsigned    pad;    /* 对齐填充：mhdr 24B，用户区 8 字节对齐（duktape duk_tval=double 需要） */
} mhdr_t;

#define M_ALIGN 16
#define M_MAGIC 0xABADCAFE

static mhdr_t *mlist;
static void  *m_heap_base;
static unsigned m_heap_size;

static unsigned m_round(unsigned x)
{
    return (x + (M_ALIGN - 1)) & ~(M_ALIGN - 1);
}

static void *m_extend(unsigned need)
{
    unsigned cur = (unsigned)sys_brk(0);
    unsigned want = m_round(cur + need);
    unsigned r = (unsigned)sys_brk((int)want);
    if (r == (unsigned)-1 || r < want) {
        { static int ext_fail = 0;
          if (ext_fail++ < 8)
              printf("[heap] EXT-FAIL cur=0x%x want=0x%x r=0x%x brk_top=0x%x\n", cur, want, r, (unsigned)sys_brk(0)); }
        return 0;
    }
    if (!m_heap_base)
        m_heap_base = (void *)cur;
    m_heap_size += r - cur;
    { static unsigned hp_log = 0;
      if (m_heap_size > hp_log + (16u << 20)) {
          hp_log = m_heap_size;
          printf("[heap] size=%uMB cur=0x%x want=0x%x\n", m_heap_size >> 20, cur, want);
      }
    }
    return (void *)cur;
}

/* 调试快照全局：库自身给出弱定义，具体 app（javm/js）可覆盖。
 * 这样 libaptos 不再依赖某个 app 的符号，任何 app 都能独立链接。 */
volatile const char *dbg_cls __attribute__((weak)) = 0;
volatile const char *dbg_mth __attribute__((weak)) = 0;

/* javm GC 触发用：当前堆已分配字节数（无系统调用开销） */
unsigned aptos_heap_size(void) { return m_heap_size; }

void *malloc(unsigned size)
{
    mhdr_t *m, *best = 0;
    unsigned need = m_round(size + sizeof(mhdr_t));
    { static int big_log = 0;
      if (size > (1u << 20) && big_log++ < 10)
          printf("[heap] BIG malloc %u bytes (%uMB) cls=%s mth=%s\n", size,
                 size >> 20, dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
    }

    if (size == 0)
        size = 1;
    if (size == 1) {
        static int m1_log = 0;
        if (m1_log++ < 8)
            printf("[heap] malloc(1) ra=0x%x cls=%s mth=%s\n",
                   (unsigned)__builtin_return_address(0),
                   dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
    }
    for (m = mlist; m; m = m->next) {
        if ((unsigned)m < 0xc0000000u || (m->next && (unsigned)m->next < 0xc0000000u)) {
            /* 链表损坏：修复（截断链），不重置（防堆泄漏） */
            printf("[heap] mlist corrupt 0x%x next=0x%x size=%u alloc=%s.%s -> truncate at %s.%s\n",
                   (unsigned)m, (unsigned)m->next, m->size,
                   m->ac ? m->ac : "?", m->am ? m->am : "?",
                   dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
            /* 打印物理前一块（写穿源，含不连续容差 1KB） */
            for (mhdr_t *q = mlist; q; q = q->next) {
                if ((unsigned)q < 0xc0000000u) break; /* 链又坏：停止遍历，防二次崩溃 */
                if ((unsigned)q < (unsigned)m) {
                    unsigned end = (unsigned)q + (unsigned)sizeof(mhdr_t) + q->size;
                    if (end >= (unsigned)m - 1024u && end <= (unsigned)m) {
                        printf("[heap]   prev 0x%x size=%u free=%d alloc=%s.%s end=0x%x (m=0x%x diff=%u)\n",
                               (unsigned)q, q->size, q->free,
                               q->ac ? q->ac : "?", q->am ? q->am : "?",
                               end, (unsigned)m, (unsigned)m - end);
                        break;
                    }
                }
            }
            m->next = 0;
            break;
        }
        if (m->free && m->size >= size) {
            best = m;
            break;
        }
    }
    if (!best) {
        void *p = m_extend(need);
        if (!p)
            return 0;
        m = (mhdr_t *)p;
        m->next = mlist;
        m->size = size;
        m->free = 0;
        m->ac = dbg_cls; m->am = dbg_mth;
        mlist = m;
        best = m;
    } else {
        /* 拆分：best 物理大小 = round(size+16)，若剩余 >= 32B 切出 free 块回链（降碎片） */
        unsigned phys = m_round(best->size + sizeof(mhdr_t));
        unsigned used = m_round(size + sizeof(mhdr_t));
        if (phys >= used + sizeof(mhdr_t) + 16) {
            mhdr_t *rem = (mhdr_t *)((char *)best + used);
            rem->size = phys - used - sizeof(mhdr_t);
            rem->free = 1;
            rem->ac = 0; rem->am = 0;
            rem->next = best->next;
            best->next = rem;
            best->size = size;
        }
        best->free = 0;
        best->ac = dbg_cls; best->am = dbg_mth;
    }
    return (void *)(best + 1);
}

void free(void *ptr)
{
    mhdr_t *m;

    if (!ptr)
        return;
    if ((unsigned)ptr < 0xc0000000u || (unsigned)ptr >= 0xf0000000u)
        return; /* 野指针：忽略，防止破坏堆（堆窗 os324 起为 0xc0000000-0xf0000000） */
    m = ((mhdr_t *)ptr) - 1;
    m->free = 1;
    /* 下方合并：m->next（物理紧邻 m 下方，链降序）free 时并入 m，m 向下扩展 */
    for (;;) {
        mhdr_t *nx = m->next;
        if (!nx || !nx->free || (unsigned)nx < 0xc0000000u) break;
        if ((unsigned)m + m_round(m->size + sizeof(mhdr_t)) != (unsigned)nx) break;
        m->size += m_round(nx->size + sizeof(mhdr_t));
        m->next = nx->next;
    }
    /* 物理相邻合并：p 是 m 的链前驱（物理紧邻 m 上方）且 free 时，把 p 并入 m。
       链为物理降序（新块在头），m 在 p 下方；m 向上扩展覆盖 p 才是物理正确方向。
       旧实现把 m 并入 p（p->size 向上虚增但物理不包含 m），导致 m 链上丢失 +
       p 复用后写越界 -> 堆写穿。
       必须校验物理相邻：malloc 拆分会切出 rem 块插链，使链序与物理序错位，
       链相邻（p->next==m）不等于物理紧邻；漏校验会把中间隔块并入 m，虚增
       m->size 覆盖中间块数据（堆写穿 -> duktape/javm 链表损坏）。 */
    mhdr_t *pp = NULL;
    for (mhdr_t *p = mlist; p; pp = p, p = p->next) {
        if ((unsigned)p < 0xc0000000u)
            break;
        if (p->next == m && p->free) {
            if ((unsigned)p != (unsigned)m + m_round(m->size + sizeof(mhdr_t)))
                break; /* 链相邻但物理不紧邻（中间隔块）：禁止合并，防写穿 */
            m->size += m_round(p->size + sizeof(mhdr_t));
            if (pp) pp->next = m; else mlist = m;
            return;
        }
    }
}

void *calloc(unsigned n, unsigned sz)
{
    unsigned total = n * sz;
    void *p = malloc(total);
    if (p) {
        char *q = (char *)p;
        for (unsigned i = 0; i < total; i++)
            q[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, unsigned size)
{
    mhdr_t *m;
    void *np;

    if (!ptr)
        return malloc(size);
    if (size == 0) {
        /* 标准 realloc(ptr, 0)：释放并返回 NULL。duktape 的清理路径依赖此语义，
           若返回原指针会被误判为"内存未释放"而进入重试死循环。 */
        free(ptr);
        return NULL;
    }
    if ((unsigned)ptr < 0xc0000000u || (unsigned)ptr >= 0xf0000000u)
        return malloc(size);
    m = ((mhdr_t *)ptr) - 1;
    if (m->size >= size)
        return ptr;
    np = malloc(size);
    if (!np)
        return 0;
    {
        char *s = (char *)ptr, *d = (char *)np;
        for (unsigned i = 0; i < m->size; i++)
            d[i] = s[i];
    }
    free(ptr);
    return np;
}

void *memmove(void *dst, const void *src, unsigned n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int putchar(int c)
{
    char b = (char)c;
    sys_write(&b, 1);
    return c;
}

static int unused_putchar_marker;

int puts(const char *s)
{
    unsigned n = strlen(s);
    sys_write(s, (int)n);
    putchar('\n');
    return 0;
}

unsigned strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, unsigned n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (int)(u8)*a - (int)(u8)*b : 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *strncpy(char *d, const char *s, unsigned n)
{
    unsigned i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

void *memset(void *d, int c, unsigned n)
{
    u8 *p = (u8 *)d;
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    u8 *a = (u8 *)d;
    const u8 *b = (const u8 *)s;
    while (n--)
        *a++ = *b++;
    return d;
}

void sys_sleep_ms(int ms)
{
    sys_sleep(ms);
}

int atoi(const char *s)
{
    int v = 0, sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}

typedef struct {
    char  *out;
    int    size;
    int    n;
    int    to_stdout;
} sink_t;

static void emit(sink_t *s, char c)
{
    if (s->to_stdout) {
        sys_write(&c, 1);
    } else if (s->n + 1 < s->size) {
        s->out[s->n] = c;
    }
    s->n++;
}

static void emit_str(sink_t *s, const char *str)
{
    while (*str)
        emit(s, *str++);
}

static void emit_pad(sink_t *s, int count, char c)
{
    while (count-- > 0)
        emit(s, c);
}

static void emit_num(sink_t *s, u32 v, int base, int upper, int width, int zero, int left)
{
    char tmp[36];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, pad;

    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % (u32)base];
        v /= (u32)base;
    }
    pad = width - n;
    if (!left && !zero)
        emit_pad(s, pad, ' ');
    if (!left && zero)
        emit_pad(s, pad, '0');
    while (n--)
        emit(s, tmp[n]);
    if (left)
        emit_pad(s, pad, ' ');
}

static int format(sink_t *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        int left = 0, zero = 0, width = 0;

        if (*fmt != '%') {
            emit(s, *fmt);
            continue;
        }
        fmt++;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-')
                left = 1;
            else
                zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z')
            fmt++;

        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                emit(s, '-');
                if (width)
                    width--;
                emit_num(s, (u32)(-v), 10, 0, width, zero, left);
            } else {
                emit_num(s, (u32)v, 10, 0, width, zero, left);
            }
            break;
        }
        case 'u':
            emit_num(s, va_arg(ap, u32), 10, 0, width, zero, left);
            break;
        case 'x':
            emit_num(s, va_arg(ap, u32), 16, 0, width, zero, left);
            break;
        case 'X':
            emit_num(s, va_arg(ap, u32), 16, 1, width, zero, left);
            break;
        case 'c':
            emit(s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            int len;
            if (!str || (unsigned)str < 0xc0000000u || (unsigned)str >= 0xf0000000u)
                str = "(null)";
            len = (int)strlen(str);
            if (!left)
                emit_pad(s, width - len, ' ');
            emit_str(s, str);
            if (left)
                emit_pad(s, width - len, ' ');
            break;
        }
        case '%':
            emit(s, '%');
            break;
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void *);
            char hx[12];
            int k = 0;
            hx[k++] = '0';
            hx[k++] = 'x';
            {
                unsigned long t = v;
                int started = 0;
                for (int sh = 28; sh >= 0; sh -= 4) {
                    int dg = (int)((t >> sh) & 0xF);
                    if (dg || started || sh == 0) {
                        started = 1;
                        hx[k++] = (char)(dg < 10 ? '0' + dg : 'a' + dg - 10);
                    }
                }
            }
            hx[k] = 0;
            if (!left)
                emit_pad(s, width - k, ' ');
            emit_str(s, hx);
            if (left)
                emit_pad(s, width - k, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            /* 消费 double 参数防 va_list 错位（裸机无 %f 输出，显示 '?' 占位） */
            (void)va_arg(ap, double);
            emit(s, '?');
            break;
        }
        case 0:
            fmt--;
            break;
        default:
            /* 未知转换符：消费一个 int 参数防错位，避免后续 %s 读到垃圾指针 */
            (void)va_arg(ap, int);
            emit(s, '%');
            emit(s, *fmt);
            break;
        }
    }
    return s->n;
}

int printf(const char *fmt, ...)
{
    static char buf[512];
    sink_t s;
    va_list ap;
    int r;

    s.out = buf;
    s.size = sizeof(buf);
    s.n = 0;
    s.to_stdout = 0;
    va_start(ap, fmt);
    r = format(&s, fmt, ap);
    va_end(ap);
    {
        int idx = (s.n < (int)sizeof(buf)) ? s.n : (int)sizeof(buf) - 1;
        buf[idx] = 0;
        if (idx > 0)
            sys_write(buf, idx);
    }
    return r;
}

int vsnprintf(char *out, int size, const char *fmt, va_list ap)
{
    sink_t s;
    int r;

    s.out = out;
    s.size = size;
    s.n = 0;
    s.to_stdout = 0;
    r = format(&s, fmt, ap);
    if (size > 0) {
        int idx = (s.n < size) ? s.n : size - 1;
        out[idx] = 0;
    }
    return r;
}

int snprintf(char *out, int size, const char *fmt, ...)
{
    sink_t s;
    va_list ap;
    int r;

    s.out = out;
    s.size = size;
    s.n = 0;
    s.to_stdout = 0;
    va_start(ap, fmt);
    r = format(&s, fmt, ap);
    va_end(ap);
    if (size > 0) {
        int idx = (s.n < size) ? s.n : size - 1;
        out[idx] = 0;
    }
    return r;
}
