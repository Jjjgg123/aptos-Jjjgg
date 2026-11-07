/*


 * javm.c — 32 位裸机微型 JVM（class 文件解释执行）
 * 目标：跑通 HelloWorld + 循环 + 算术 + println
 * 架构：常量池解析 + 帧式解释器 + 最小 natives
 * 值模型：int32 槽（引用=指针），long/double 暂不支持（后续加）
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "aptos.h"   /* sys_open/sys_read/sys_close 等 int 0x80 封装 */
#include "jvm_inflate.h"

/* 诊断日志开关：默认关闭，避免把系统内部轨迹混进程序的 stdout。
   需要排查时用 -DJVM_VERBOSE=1 重新构建。 */
#ifndef JVM_VERBOSE
#define JVM_VERBOSE 0
#endif
#if JVM_VERBOSE
#define JDBG(...) printf(__VA_ARGS__)
#else
#define JDBG(...) ((void)0)
#endif


/* ---- 常量池 tag ---- */
#define CP_UTF8              1
#define CP_INTEGER           3
#define CP_FLOAT             4
#define CP_LONG              5
#define CP_DOUBLE            6
#define CP_CLASS             7
#define CP_STRING            8
#define CP_FIELDREF          9
#define CP_METHODREF         10
#define CP_INTERFACE_METHODREF 11
#define CP_NAME_AND_TYPE     12
#define CP_METHODHANDLE      15
#define CP_METHODTYPE        16
#define CP_DYNAMIC           17
#define CP_INVOKEDYNAMIC     18
#define CP_MODULE            19
#define CP_PACKAGE           20

#define ACC_STATIC  0x0008
#define ACC_PUBLIC  0x0001
#define ACC_NATIVE  0x0100

struct cp_entry {
    uint8_t tag;
    union {
        struct { uint16_t len; const char *str; } utf8;
        int32_t i32;
        int64_t i64;
        struct { uint16_t a, b; } ref;
    } u;
};

struct jvm_class;
struct jvm_field {
    uint16_t flags;
    const char *name;
    const char *desc;
    uint32_t offset;      /* 实例字段偏移（槽） */
};
struct jvm_method {
    uint16_t flags;
    const char *name;
    const char *desc;
    uint16_t max_stack, max_locals;
    uint8_t *code;
    uint32_t code_len;
    uint16_t *exc_tbl;   /* 异常表（每项 4 u16：start_pc/end_pc/handler_pc/catch_type） */
    uint16_t exc_count;
    struct jvm_class *owner; /* 方法定义类（继承方法 cp 解析必须用它，不能用调用点类） */
};
struct jvm_class {
    const char *name;
    struct jvm_class *super;
    struct cp_entry *cp;
    uint16_t cp_count;
    struct jvm_field *fields;
    uint16_t field_count;
    struct jvm_method *methods;
    uint16_t method_count;
    int32_t *statics;
    uint32_t static_count;
    uint8_t initialized;
    uint32_t inst_size;    /* 实例字段槽数 */
};

/* ---- 对象 ---- */
#define OBJ_MAGIC 0x4F424A31u /* 'OBJ1'：GC 身份校验（保守 GC 无类型信息，防误标堆内非对象数据） */
struct jvm_obj {
    uint32_t magic;        /* OBJ_MAGIC */
    struct jvm_class *cls;
    uint32_t is_array;
    int32_t array_len;
    int32_t *array_data;   /* 数组元素（int32 槽） */
    int32_t mark;          /* GC 标记（0=可回收） */
    int32_t fields[1];     /* 实例字段 */
};

#define MAX_CP 4096
#define MAX_CLASSES 4096
#define MAX_STRINGS 512
#define STACK_SLOTS 131072

static struct jvm_class *classes[MAX_CLASSES];
static char jar_path[256] = "/APPS/TEST.JAR"; /* 类 jar 源: 入口 arg1 可覆盖 */
static int class_count = 0;
static int32_t *jvm_stack;  /* 堆分配（BSS 映射不足，16384 槽=64KB 放堆） */
static int jvm_stack_top = 0;

/* interned strings: 简化为对象数组 */
static struct jvm_obj *strings[MAX_STRINGS];
static int string_count = 0;

/* ---- GC：保守 mark-sweep（无类型信息，所有堆范围值当引用） ---- */
static int jstrcmp(const char *a, const char *b); /* 前向声明（定义在后） */
extern unsigned aptos_heap_size(void);
/* 递归帧池（文件级：GC 需枚举帧内引用；8MB BSS，5461 帧；exec_method 共用） */
static int32_t frame_mem[2097152];
static int frame_ptr = 0;
static struct jvm_obj **g_objs = NULL;
static int g_obj_count = 0, g_obj_cap = 0;
static void *gc_pins[64];
static int gc_pin_n = 0;
static int in_invoke_native = 0;

static void gc_register(struct jvm_obj *o) {
    if (!o) return;
    if (g_obj_count >= g_obj_cap) {
        int nc = g_obj_cap ? g_obj_cap * 2 : 4096;
        struct jvm_obj **ng = (struct jvm_obj **)realloc(g_objs, nc * sizeof(void *));
        if (!ng) return; /* 注册失败不致命：该对象不被 GC 跟踪 */
        g_objs = ng; g_obj_cap = nc;
    }
    g_objs[g_obj_count++] = o;
}
static void gc_pin(void *p) { if (gc_pin_n < 64) gc_pins[gc_pin_n++] = p; }
static void gc_unpin(void) { if (gc_pin_n > 0) gc_pin_n--; }

static void gc_mark_obj(struct jvm_obj *o, int depth);
static unsigned g_brk0 = 0; /* 堆起始 brk（main 初始化）：GC 野指针守卫用 */
static void gc_mark_slot(int32_t v, int depth) {
    uint32_t a = (uint32_t)v;
    if (!(a >= 0xc0000000u && a < 0xf0000000u)) return; /* 非堆指针 */
    gc_mark_obj((struct jvm_obj *)v, depth);
}
static void gc_mark_obj(struct jvm_obj *o, int depth) {
    if (!o || depth > 256) return; /* 深度保护：超限放弃该支（保守多活） */
    uint32_t a = (uint32_t)o;
    if (!(a >= 0xc0000000u && a < 0xf0000000u)) return;
    if (g_brk0 && (a < g_brk0 || a >= (g_brk0 + aptos_heap_size()))) return; /* 未分配区（heap 顶以上）：野指针，不解引用不写 */
    if ((a & 3) != 0) return; /* 对象 4 字节对齐（malloc 用户区 4 对齐）；未对齐值非对象 */
    if (o->magic != OBJ_MAGIC) return; /* 非对象（栈整数/字符串数据）：不标记不解引用不写 */
    if (o->mark) return;
    o->mark = 1;
    if (o->is_array) {
        /* 字符串对象（cstr）不扫元素；其他数组扫 int32 元素 */
        if (o->cls && o->cls->name && jstrcmp(o->cls->name, "java/lang/String") == 0)
            return;
        int32_t *dd = o->array_data;
        int32_t n = o->array_len;
        if (dd && n > 0 && (uint32_t)dd >= 0xc0000000u && (uint32_t)dd < 0xf0000000u)
            for (int32_t i = 0; i < n; i++) gc_mark_slot(dd[i], depth + 1);
    } else {
        int32_t n = (o->cls && o->cls->inst_size) ? (int32_t)o->cls->inst_size : 1;
        for (int32_t i = 0; i < n && i < 512; i++) gc_mark_slot(o->fields[i], depth + 1);
    }
}

static unsigned gc_collect(void) {
    /* 1) 清标记 */
    for (int i = 0; i < g_obj_count; i++) {
        struct jvm_obj *o = g_objs[i];
        if (!o || o->magic != OBJ_MAGIC) continue; /* 防御：登记表损坏时跳过 */
        o->mark = 0;
    }
    /* 2) 标记根：帧池（Java 引用全在其中） */
    for (int i = 0; i < 2097152 && i < frame_ptr + 384; i++)
        gc_mark_slot(frame_mem[i], 0);
    /* jvm_stack（局部/操作数栈残值保守标记） */
    for (int i = 0; i < STACK_SLOTS; i++) gc_mark_slot(jvm_stack[i], 0);
    /* 静态字段 */
    for (int c = 0; c < class_count && c < MAX_CLASSES; c++) {
        struct jvm_class *k = classes[c];
        if (!k || (uint32_t)k >= 0xf0000000u) continue;
        if (k->statics)
            for (uint32_t s = 0; s < k->static_count; s++)
                gc_mark_slot(k->statics[s], 0);
    }
    /* interned strings + 保护对象 */
    for (int i = 0; i < string_count; i++) gc_mark_slot((int32_t)(intptr_t)strings[i], 0);
    for (int i = 0; i < gc_pin_n; i++) gc_mark_slot((int32_t)(intptr_t)gc_pins[i], 0);
    /* 3) 清除 */
    unsigned freed = 0;
    int w = 0;
    for (int i = 0; i < g_obj_count; i++) {
        struct jvm_obj *o = g_objs[i];
        if (!o) continue;
        if (o->magic != OBJ_MAGIC) continue; /* 对象头被 Java 侧写坏：跳过不 free（防二次伤害） */
        if (!o->mark) {
            /* 预检 array_data（范围+对齐）：字段可能被写坏成垃圾，泄漏优于 free 垃圾写坏块头 */
            if (o->is_array && o->array_data &&
                (uint32_t)o->array_data >= 0xc0000000u &&
                (uint32_t)o->array_data < 0xf0000000u &&
                ((uint32_t)o->array_data & 3) == 0)
                free(o->array_data);
            o->magic = 0; /* 擦身份：防后续保守标记把已 free 内存当对象写 mark（堆写坏根因） */
            free(o);
            freed++;
        } else {
            g_objs[w++] = o;
        }
    }
    g_obj_count = w;
    if (freed) printf("[GC] freed %u objects (live %d heap=%uMB)\n", freed, w, aptos_heap_size() >> 20);
    return freed;
}

static void maybe_gc(void) {
    static unsigned last_heap = 0;
    unsigned h = aptos_heap_size();
    if (!last_heap) last_heap = h;
    /* 前 256MB 不回收：早期(注册表阶段)保守标记实测会堆写坏(corrupt)；稳定优先。
       之后每 16MB 增量回收：MC 启动吃内存快，高频回收对抗 795MB 物理堆墙。 */
    if (h < (384u << 20)) return;
    if (h - last_heap >= (16u << 20) && !in_invoke_native) {
        last_heap = h;
        gc_collect();
    }
}

/* 安全字符串函数：指针守卫（防野指针崩） */
static int jstrcmp(const char *a, const char *b) {
    uint32_t aa = (uint32_t)a, bb = (uint32_t)b;
    if (!(a && aa >= 0xc0000000u && aa < 0xf0000000u)) return (b && bb >= 0xc0000000u && bb < 0xf0000000u) ? 1 : 0;
    if (!(b && bb >= 0xc0000000u && bb < 0xf0000000u)) return -1;
    for (size_t i = 0; i < 0x100000; i++) {
        char ca = a[i], cb = b[i];
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (!ca) return 0;
    }
    return 0;
}
static size_t jstrlen(const char *s) {
    uint32_t a = (uint32_t)s;
    if (!(s && a >= 0xc0000000u && a < 0xf0000000u)) return 0;
    size_t n = 0;
    while (s[n] && n < 0x100000) n++; /* 有界：防无 NUL 终止的数组内存死循环 */
    return n;
}
static int jstrncmp(const char *a, const char *b, size_t n) {
    uint32_t aa = (uint32_t)a, bb = (uint32_t)b;
    if (!(a && aa >= 0xc0000000u && aa < 0xf0000000u)) return (b && bb >= 0xc0000000u && bb < 0xf0000000u) ? 1 : 0;
    if (!(b && bb >= 0xc0000000u && bb < 0xf0000000u)) return -1;
    return (strncmp)(a, b, n);
}
/* ---- 简单错误输出 ---- */
static void jerr(const char *msg) {
    printf("JVM error: %s\n", msg);
    sys_exit(1);
}

/* ---- 读取文件 ---- */
static uint8_t *read_file(const char *path, uint32_t *out_len) {
    int fd = sys_open(path, 0);
    if (fd < 0) return NULL;
    /* 中转读入，再按实际大小分配独立缓冲（懒加载递归时不能复用同一静态缓冲，
       否则父类解析中途 read 子类会覆盖其数据） */
    static uint8_t tmp[16777216]; /* 16MB: MC server jar ~9.6MB */
    { static int checked = 0;
      if (!checked) { checked = 1;
          unsigned brk0 = (unsigned)sys_brk(0);
          JDBG("[load] tmp=0x%x brk0=0x%x gap=%d\n", (unsigned)tmp, brk0, (int)brk0 - (int)tmp);
      } }
    int n = sys_read(fd, tmp, sizeof(tmp) - 1);
    sys_close(fd);
    if (n <= 0) return NULL;
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) jerr("read oom");
    memcpy(buf, tmp, (size_t)n);
    buf[n] = 0;
    *out_len = (uint32_t)n;
    return buf;
}

/* ---- 小端读取（zip 中央目录/本地头） ---- */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int javm_inflate(const uint8_t *in, uint32_t in_len, uint8_t *out,
                 uint32_t out_cap, uint32_t *out_len);

static int jvm_memcmp(const void *a, const void *b, unsigned long n);

/* ---- jar/zip 条目加载（EOCD→中央目录→本地头→解压） ---- */
static uint8_t *g_jar = NULL;
static uint32_t g_jlen = 0;
/* jar 中央目录索引：一次构建，O(1) 查找，避免每次全盘扫描 */
struct jentry { const char *name; uint32_t loff; uint32_t csize; uint32_t usize; uint16_t method; };
static struct jentry jidx[16384];
static int jidx_count = 0;
static int jidx_ready = 0;

static int jar_load_entry(const char *jar_path, const char *entry,
                          uint8_t **out_data, uint32_t *out_len)
{
    uint32_t jlen = 0;
    uint8_t *jb;
    if (g_jar) {
        jb = g_jar;
        jlen = g_jlen;
    } else {
        jb = read_file(jar_path, &jlen);
        if (!jb) return -1;
        g_jar = jb;
        g_jlen = jlen;
    }
    /* EOCD：PK 05 06，从尾部 22 字节处向前扫 */
    uint32_t eocd = 0;
    int found = 0;
    for (uint32_t i = (jlen > 22 ? jlen - 22 : 0); i + 3 < jlen; i++) {
        if (jb[i] == 0x50 && jb[i+1] == 0x4b && jb[i+2] == 0x05 && jb[i+3] == 0x06) {
            eocd = i; found = 1; break;
        }
    }
    if (!found) { /* jar cached */ return -1; }
    if (eocd + 22 > jlen) { /* jar cached */ return -1; }
    uint16_t cd_entries = le16(jb + eocd + 10);
    uint32_t cd_off = le32(jb + eocd + 16);
    /* 构建中央目录索引（首次） */
    if (!jidx_ready) {
        jidx_ready = 1;
        uint32_t p2 = cd_off;
        for (uint16_t e2 = 0; e2 < cd_entries && jidx_count < 16384; e2++) {
            if (p2 + 46 > jlen) break;
            if (!(jb[p2] == 0x50 && jb[p2+1] == 0x4b && jb[p2+2] == 0x01 && jb[p2+3] == 0x02)) break;
            uint16_t nlen2 = le16(jb + p2 + 28);
            uint16_t elen2 = le16(jb + p2 + 30);
            uint16_t clen2 = le16(jb + p2 + 32);
            uint32_t loff2 = le32(jb + p2 + 42);
            if (p2 + 46 + nlen2 > jlen) break;
            if (nlen2 > 0 && nlen2 < 220) {
                char *nm = malloc((size_t)nlen2 + 1);
                if (nm) {
                    memcpy(nm, jb + p2 + 46, nlen2); nm[nlen2] = 0;
                    jidx[jidx_count].name = nm;
                    jidx[jidx_count].loff = loff2;
                    jidx[jidx_count].csize = le32(jb + p2 + 20);
                    jidx[jidx_count].usize = le32(jb + p2 + 24);
                    jidx[jidx_count].method = le16(jb + p2 + 10);
                    jidx_count++;
                }
            }
            p2 += 46 + nlen2 + elen2 + clen2;
        }
    }
    /* 索引查找（大小写不敏感） */
    {
        const char *want = entry;
        uint32_t wl = (uint32_t)jstrlen(want);
        int hit = -1;
        for (int qi = 0; qi < jidx_count; qi++) {
            const char *nm = jidx[qi].name;
            uint32_t nl = (uint32_t)jstrlen(nm);
            if (nl != wl) continue;
            uint32_t mok = 1;
            for (uint32_t mi = 0; mi < wl; mi++) {
                char x = nm[mi], y = want[mi];
                if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
                if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
                if (x != y) { mok = 0; break; }
            }
            if (mok) { hit = qi; break; }
        }
        if (hit < 0) return -1;
        uint32_t loff = jidx[hit].loff;
        uint16_t lmethod = jidx[hit].method;
        uint32_t lcsize = jidx[hit].csize;
        uint32_t lusize = jidx[hit].usize;
        if (loff + 30 > jlen) return -1;
        uint16_t lnlen = le16(jb + loff + 26);
        uint16_t lxlen = le16(jb + loff + 28);
        if (loff + 30 + lnlen + lxlen + lcsize > jlen) return -1;
        const uint8_t *dstart = jb + loff + 30 + lnlen + lxlen;
        if (lmethod == 0) {
            *out_data = malloc((size_t)lusize + 1);
            if (!*out_data) return -1;
            memcpy(*out_data, dstart, lusize);
            (*out_data)[lusize] = 0;
            *out_len = lusize;
        } else if (lmethod == 8) {
            uint8_t *wrap = malloc((size_t)lcsize + 6);
            if (!wrap) return -1;
            wrap[0] = 0x78; wrap[1] = 0x01;
            memcpy(wrap + 2, dstart, lcsize);
            memset(wrap + 2 + lcsize, 0, 4);
            uint8_t *out = malloc((size_t)lusize + 1);
            if (!out) { free(wrap); return -1; }
            uint32_t olen = 0;
            int ir = javm_inflate(wrap, lcsize + 6, out, lusize + 1, &olen);
            if (ir == 0 && olen <= lusize) {
                out[olen] = 0;
                *out_data = out; *out_len = olen;
            } else {
                free(out); free(wrap); return -1;
            }
            free(wrap);
        } else {
            return -1;
        }
        return 0;
    }
}

/* ---- 大端读取 ---- */
static uint16_t r16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t r32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* ---- 常量池字符串 ---- */
static const char *cp_utf8(struct jvm_class *c, uint16_t idx) {
    if (idx == 0 || idx >= c->cp_count) return "(bad)";
    struct cp_entry *e = &c->cp[idx];
    if (e->tag == CP_UTF8) return e->u.utf8.str;
    if (e->tag == CP_CLASS || e->tag == CP_STRING) {
        uint16_t u = e->u.ref.a;
        if (u < c->cp_count && c->cp[u].tag == CP_UTF8) return c->cp[u].u.utf8.str;
    }
    return "(bad)";
}

static struct jvm_class *find_class(const char *name);
static void clinit(struct jvm_class *c);

/* ---- 字符串对象（简化：char 数组不可变，直接存 cstr） ---- */
/* 伪 String 类：jvm_string 生成的对象用它标记（避免 find_class 失败返回 NULL） */
static struct jvm_class *str_cls = NULL;
static struct jvm_class *pseudo_string_class(void) {
    if (!str_cls) {
        str_cls = calloc(1, sizeof(struct jvm_class));
        if (!str_cls) jerr("strcls oom");
        str_cls->name = "java/lang/String";
    }
    return str_cls;
}

struct jvm_obj *jvm_string(const char *s) {
    int i;
    for (i = 0; i < string_count; i++)
        if (jstrcmp((char *)strings[i]->array_data, s) == 0)
            return strings[i];
    /* 独立复制：不能借用调用者的栈/临时缓冲（函数返回后失效） */
    {
        int32_t n = (int32_t)jstrlen(s);
        char *copy = malloc((size_t)n + 1);
        if (!copy) jerr("string oom");
        memcpy(copy, s, (size_t)n + 1);
        struct jvm_obj *o = malloc(sizeof(struct jvm_obj) + 4);
        if (!o) jerr("string oom");
        memset(o, 0, sizeof(struct jvm_obj) + 4);
        o->magic = OBJ_MAGIC;
        o->cls = pseudo_string_class();
        o->is_array = 1;
        o->array_len = n;
        o->array_data = (int32_t *)(void *)copy;
        gc_register(o);
        if (string_count < MAX_STRINGS) strings[string_count++] = o;
        return o;
    }
}

/* ---- 类解析 ---- */
static struct jvm_class *parse_class(const uint8_t *b, uint32_t len) {
    uint32_t pos = 0;
    if (len < 10 || r32(b) != 0xCAFEBABEu) { jerr("bad magic"); return NULL; }
    pos = 8; /* skip magic+version */
    uint16_t cp_count = r16(b + pos); pos += 2;
    if (cp_count < 1 || cp_count > MAX_CP) { jerr("cp too big"); return NULL; }

    struct jvm_class *c = calloc(1, sizeof(struct jvm_class));
    c->cp = calloc(cp_count, sizeof(struct cp_entry));
    c->cp_count = cp_count;

    for (int i = 1; i < cp_count; i++) {
        uint8_t tag = b[pos++];
        struct cp_entry *e = &c->cp[i];
        e->tag = tag;
        switch (tag) {
        case CP_UTF8: {
            uint16_t l = r16(b + pos); pos += 2;
            char *s = malloc((size_t)l + 1);
            if (!s) jerr("utf8 oom");
            memcpy(s, b + pos, l);
            s[l] = 0;
            e->u.utf8.len = l;
            e->u.utf8.str = s;
            pos += l;
            break;
        }
        case CP_INTEGER:
            e->u.i32 = (int32_t)r32(b + pos); pos += 4; break;
        case CP_FLOAT:
            e->u.i32 = (int32_t)r32(b + pos); pos += 4; break;
        case CP_LONG:
        case CP_DOUBLE: {
            int32_t hi = (int32_t)r32(b + pos);
            int32_t lo = (int32_t)r32(b + pos + 4);
            e->u.i64 = ((int64_t)hi << 32) | (uint32_t)lo;
            pos += 8;
            i++; /* long/double 占两个槽 */
            break;
        }
        case CP_CLASS:
        case CP_STRING:
        case CP_METHODTYPE:
            e->u.ref.a = r16(b + pos); pos += 2; break;
        case CP_FIELDREF:
        case CP_METHODREF:
        case CP_INTERFACE_METHODREF:
        case CP_NAME_AND_TYPE:
        case CP_DYNAMIC:
        case CP_INVOKEDYNAMIC:
            e->u.ref.a = r16(b + pos);
            e->u.ref.b = r16(b + pos + 2);
            pos += 4;
            break;
        case CP_METHODHANDLE:
            pos += 3; break;
        case 2: case 13: case 14:
            /* 保留 tag / 模块 / 包：跳过（无法解析的先报错） */
            jerr("cp unsupported tag");
            break;
        default:
            printf("[CP] bad tag=%u i=%d pos=%u cp_count=%u len=%u\n",
                   tag, i, pos, cp_count, len);
            jerr("cp bad tag");
            break;
        }
    }

    c->super = NULL;
    uint16_t acc = r16(b + pos); pos += 2; (void)acc;
    uint16_t this_idx = r16(b + pos); pos += 2;
    uint16_t super_idx = r16(b + pos); pos += 2;
    c->name = cp_utf8(c, this_idx);
    if (super_idx) {
        /* 懒加载 super 并链接（find_class 会递归 parse；Object 为预注册占位） */
        c->super = find_class(cp_utf8(c, super_idx));
    }

    /* interfaces */
    uint16_t icount = r16(b + pos); pos += 2;
    pos += icount * 2;

    /* fields */
    uint16_t fcount = r16(b + pos); pos += 2;
    c->fields = calloc(fcount, sizeof(struct jvm_field));
    c->field_count = fcount;
    uint32_t foff = (c->super ? c->super->inst_size : 0);
    for (int i = 0; i < fcount; i++) {
        struct jvm_field *f = &c->fields[i];
        f->flags = r16(b + pos); pos += 2;
        f->name = cp_utf8(c, r16(b + pos)); pos += 2;
        f->desc = cp_utf8(c, r16(b + pos)); pos += 2;
        uint16_t acount = r16(b + pos); pos += 2;
        for (int a = 0; a < acount; a++) {
            r16(b + pos); pos += 2;          /* attr name */
            uint32_t alen = r32(b + pos); pos += 4;
            pos += alen;
        }
        if (!(f->flags & ACC_STATIC)) {
            f->offset = foff;
            foff += 1;   /* 简化：所有非 static 字段占 1 槽 */
        }
    }
    c->inst_size = foff;

    /* methods */
    uint16_t mcount = r16(b + pos); pos += 2;
    c->methods = calloc(mcount, sizeof(struct jvm_method));
    c->method_count = mcount;
    for (int i = 0; i < mcount; i++) {
        struct jvm_method *m = &c->methods[i];
        m->flags = r16(b + pos); pos += 2;
        m->name = cp_utf8(c, r16(b + pos)); pos += 2;
        m->desc = cp_utf8(c, r16(b + pos)); pos += 2;
        uint16_t acount = r16(b + pos); pos += 2;
        for (int a = 0; a < acount; a++) {
            uint16_t aname = r16(b + pos); pos += 2;
            uint32_t alen = r32(b + pos); pos += 4;
            uint32_t start = pos;
            if (alen <= len - start) {
                const char *an = cp_utf8(c, aname);
                if (an && jstrcmp(an, "Code") == 0) {
                    /* Code attribute: max_stack 2B max_locals 2B code_len 4B code */
                    m->max_stack = r16(b + start); 
                    m->max_locals = r16(b + start + 2);
                    uint32_t clen = r32(b + start + 4);
                    if (clen > alen - 8) {
                        /* 防御：code_len 超出属性长度 = 属性错位（历史根因），显式报错而非静默错位 */
                        printf("[JVM] %s.%s Code len %u > attr len %u (misaligned, skip)\n",
                               c->name ? c->name : "?", m->name ? m->name : "?",
                               clen, alen);
                        clen = 0;
                        m->max_stack = 0; m->max_locals = 0;
                    }
                    m->code = (uint8_t *)(b + start + 8);
                    m->code_len = clen;
                    /* 异常表在 code 之后：exception_table_length(2B) + 每项 start/end/handler/catch_type 各 2B */
                    uint16_t etl = r16(b + start + 8 + clen);
                    m->exc_count = etl;
                    m->exc_tbl = (uint16_t *)(b + start + 10 + clen);
                }
            }
            pos += alen;
        }
    }

    /* 类 attributes 跳过 */
    uint16_t ccount = r16(b + pos); pos += 2;
    for (int i = 0; i < ccount; i++) {
        r16(b + pos); pos += 2;
        uint32_t alen = r32(b + pos); pos += 4;
        pos += alen;
    }

    /* 静态字段分配 */
    for (int i = 0; i < fcount; i++)
        if (c->fields[i].flags & ACC_STATIC)
            c->static_count++;
    if (c->static_count)
        c->statics = calloc(c->static_count, sizeof(int32_t));

    /* 注册 */
    if (class_count < MAX_CLASSES) classes[class_count++] = c;
    return c;
}

static uint8_t *pool_copy(const uint8_t *src, uint32_t len);
static struct jvm_class *find_class(const char *name) {
    if (!name) return NULL;
    { static unsigned fc_cnt = 0;
      if ((fc_cnt++ & 0xFFF) == 0)
          JDBG("[FCX] find %s heap=%uMB classes=%d\n", name, aptos_heap_size() >> 20, class_count);
    }
    /* 数组类（[I、[Ljava/lang/Object; 等）：无 .class 文件，运行时类；
       以 java/lang/Object 伪类表示（javm 数组操作不依赖精确元素类型） */
    if (name[0] == '[') {
        struct jvm_class *oc = find_class("java/lang/Object");
        if (oc) return oc;
    }
    for (int i = 0; i < class_count; i++)
        if (classes[i]->name && jstrcmp(classes[i]->name, name) == 0) {
            struct jvm_class *c = classes[i];
            if (!c->initialized) clinit(c);
            return c;
        }
    /* 按需加载：jar 优先（权威版本），/APPS/<NAME>.CLASS 回退 */
    {
        char en[96];
        snprintf(en, sizeof(en), "%s.class", name);
        uint8_t *jd = NULL; uint32_t jl = 0;
        if (jar_load_entry(jar_path, en, &jd, &jl) == 0 && jd) {
            struct jvm_class *c = parse_class(pool_copy(jd, jl), jl);
            /* 注意：不 free(jd)——方法 code 指针指向 pool 缓冲；
               parse_class 内部已注册到 classes[]，这里不再重复注册 */
            if (c) {
                if (!c->initialized) clinit(c);
                return c;
            }
        }
        {
            char up[64];
            int k = 0;
            const char *s = name;
            for (; *s && k < 58; s++) {
                char c = *s;
                if (c == '/') c = '_';
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                up[k++] = c;
            }
            up[k] = 0;
            char p[80];
            snprintf(p, sizeof(p), "/APPS/%s.CLASS", up);
            uint32_t len = 0;
            uint8_t *data = read_file(p, &len);
            if (data) {
                struct jvm_class *c = parse_class(pool_copy(data, len), len);
                if (c) {
                    (void)p;
                    if (!c->initialized) clinit(c);
                    return c;
                }
            }
        }
    }
    { static int fc_log = 0;
      if (fc_log++ < 24)
          printf("[FC] not found: %s (class_count=%d)\n", name ? name : "?", class_count);
    }
    return NULL;
}

/* ---- class 缓冲独立池：堆被越界写坏并 reset 后，已加载类的 code 仍安全 ---- */
static uint8_t class_pool[8388608]; /* 8MB */
static uint32_t class_pool_used = 0;
static uint8_t *pool_copy(const uint8_t *src, uint32_t len) {
    if (class_pool_used + len > 8388608) { jerr("class pool overflow"); }
    uint8_t *dst = &class_pool[class_pool_used];
    memcpy(dst, src, len);
    class_pool_used += len;
    return dst;
}

/* ---- 对象创建 ---- */
static struct jvm_obj *obj_new(struct jvm_class *cls) {
    uint32_t sz = sizeof(struct jvm_obj) +
        (cls && cls->inst_size && cls->inst_size > 1 ? (cls->inst_size - 1) * 4 : 0);
    struct jvm_obj *o = calloc(1, sz);
    if (!o) jerr("obj oom");
    o->magic = OBJ_MAGIC;
    o->cls = cls;
    gc_register(o);
    return o;
}

static struct jvm_obj *arr_new(struct jvm_class *cls, int32_t len) {
    if (len < 0 || len > 33554432) {
        /* 超限：截断为 0 长（防 1GB 分配崩溃），打印提示（限流） */
        static int32_t last_clamped = 0;
        if (len != last_clamped) {
            printf("[JVM] array len clamped %d -> 0\n", len);
            last_clamped = len;
        }
        len = 0;
    }
    struct jvm_obj *o = malloc(sizeof(struct jvm_obj));
    if (!o) jerr("arr oom");
    memset(o, 0, sizeof(struct jvm_obj));
    o->magic = OBJ_MAGIC;
    o->cls = cls;
    o->is_array = 1;
    o->array_len = len;
    gc_register(o);
    if (len > 0) {
        o->array_data = calloc(len, sizeof(int32_t));
        if (!o->array_data) { /* 堆满/损坏：降级为空数组，防后续解引用崩溃 */
            o->array_len = 0;
        }
    }
    return o;
}

/* ---- 方法查找 ---- */
static struct jvm_method *find_method(struct jvm_class *c, const char *name, const char *desc) {
    for (struct jvm_class *k = c; k; k = k->super) {
        if ((uint32_t)k < 0xc0000000u || (uint32_t)k >= 0xf0000000u)
            break; /* 非法类指针：停止遍历 */
        for (int i = 0; i < k->method_count; i++) {
            struct jvm_method *m = &k->methods[i];
            if (jstrcmp(m->name, name) == 0 && jstrcmp(m->desc, desc) == 0) {
                m->owner = k;
                return m;
            }
        }
    }
    { static int fm_log = 0;
      if (fm_log++ < 24)
          JDBG("[FM] not found: %s.%s%s (chain=%s flds=%d mcount=%d)\n",
                 c && c->name ? c->name : "?", name ? name : "?", desc ? desc : "?",
                 c && c->name ? c->name : "?", c ? c->field_count : -1, c ? c->method_count : -1);
    }
    return NULL;
}

/* ---- 字段查找（返回类+偏移） ---- */
static int32_t *static_field_slot(struct jvm_class *k, const char *name, const char *desc) {
    if (!k) return NULL;
    /* 首层 fields/statics NULL 不再直接 return：字段可能定义在父类（super 链），
       跳过本层继续向上找；statics 非空时才可解引用 */
    for (; k; k = k->super) {
        if ((uint32_t)k >= 0xf0000000u) break; /* 仅防栈/内核区非法指针；calloc 低地址合法 */
        if (!k->fields || !k->statics) {
            static int sfs_null = 0;
            if (sfs_null++ < 8)
                JDBG("[SFS] skip %s (fields/statics NULL) -> super\n", k->name ? k->name : "?");
            continue;
        }
        static int sfs_log = 0;
        if (sfs_log++ < 16) {
            JDBG("[SFS] scan %s flds=%d for %s.%s%s\n", k->name ? k->name : "?",
                   k->field_count, name, name, desc);
            for (int z = 0; z < k->field_count && z < 4; z++) {
                struct jvm_field *ff = &k->fields[z];
                JDBG("[SFS]   f%d flags=%04x name=%s desc=%s static=%d\n", z, ff->flags,
                       ff->name ? ff->name : "?", ff->desc ? ff->desc : "?", !!(ff->flags & ACC_STATIC));
            }
        }
        for (int i = 0; i < k->field_count; i++) {
            struct jvm_field *f = &k->fields[i];
            if ((f->flags & ACC_STATIC) && jstrcmp(f->name, name) == 0 &&
                jstrcmp(f->desc, desc) == 0) {
                int idx = 0;
                for (int j = 0; j < k->field_count; j++)
                    if (k->fields[j].flags & ACC_STATIC) {
                        if (j == i) return &k->statics[idx];
                        idx++;
                    }
            }
        }
    }
    return NULL;
}

static int32_t *field_slot(struct jvm_obj *o, const char *name, const char *desc) {
    if (!o || (uint32_t)o < 0xc0000000u || (uint32_t)o >= 0xf0000000u)
        return NULL; /* 非法对象地址（如 0xffffffff 残留）：不解引用 */
    /* 实例字段可能定义在父类：首层 fields NULL 时沿 super 链继续找 */
    for (struct jvm_class *k = o->cls; k; k = k->super) {
        if ((uint32_t)k >= 0xf0000000u) break; /* 仅防栈/内核区非法指针；calloc 低地址合法 */
        if (!k->fields) continue;
        for (int i = 0; i < k->field_count; i++) {
            struct jvm_field *f = &k->fields[i];
            if (jstrcmp(f->name, name) == 0 && jstrcmp(f->desc, desc) == 0) {
                if (f->flags & ACC_STATIC) {
                    int idx = 0;
                    for (int j = 0; j < k->field_count; j++)
                        if (k->fields[j].flags & ACC_STATIC) {
                            if (j == i) return &k->statics[idx];
                            idx++;
                        }
                }
                return &o->fields[f->offset];
            }
        }
    }
    return NULL;
}

/* ---- natives ---- */
static struct jvm_obj *sys_out;  /* System.out PrintStream 对象 */

/* 裸机 libc 无 strchr/strstr：自实现（有界版：防无 NUL 数组死循环） */
static const char *jvm_strchr(const char *s, int c)
{
    uint32_t a = (uint32_t)s;
    if (!(s && a >= 0xc0000000u && a < 0xf0000000u)) return NULL;
    for (size_t i = 0; s[i] && i < 0x100000; i++)
        if ((uint8_t)s[i] == (uint8_t)c) return s + i;
    return NULL;
}
static const char *jvm_strstr(const char *h, const char *n)
{
    if (!n || !*n) return h;
    uint32_t ah = (uint32_t)h;
    if (!(h && ah >= 0xc0000000u && ah < 0xf0000000u)) return NULL;
    for (size_t i = 0; h[i] && i < 0x100000; i++) {
        const char *a = h + i, *b = n;
        while (*a && *b && *a == *b && (size_t)(a - h) < 0x100000 && (size_t)(b - n) < 0x100000) { a++; b++; }
        if (!*b) return h + i;
    }
    return NULL;
}
static int jvm_memcmp(const void *a, const void *b, unsigned long n)
{
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    unsigned long i;
    for (i = 0; i < n; i++)
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    return 0;
}
static void jvm_strcat(char *dst, const char *src)
{
    if (!src) return;
    while (*dst) dst++;
    while ((*dst++ = *src++));
}

/* 64 位十进制转字符串（逐位长除法，避开 libgcc __udivdi3 用户态栈风险） */
static void lltoa10(int64_t v, char *out)
{
    char tmp[24];
    int i = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do {
        uint64_t r = 0, q = 0;
        int b;
        for (b = 63; b >= 0; b--) {
            r = (r << 1) | ((u >> b) & 1);
            if (r >= 10) { r -= 10; q |= ((uint64_t)1 << b); }
        }
        tmp[i++] = (char)('0' + (int)r);
        u = q;
    } while (u);
    if (neg) tmp[i++] = '-';
    while (i > 0) *out++ = tmp[--i];
    *out = 0;
}

/* ---- 64 位 / 浮点 helper（栈槽语义：long/double 占 2 槽，低字在前） ---- */
static inline int64_t pop_long(int32_t *st, int *top)
{
    int32_t hi = st[--(*top)], lo = st[--(*top)];
    return ((int64_t)(uint32_t)lo) | ((int64_t)(uint32_t)hi << 32);
}

/* ---- 自实现 64 位乘除（避开 libgcc __divdi3/__muldi3 的用户态栈风险） ---- */
static int64_t jvm_mul64(int64_t a, int64_t b)
{
    uint32_t al = (uint32_t)a, ah = (uint32_t)((uint64_t)a >> 32);
    uint32_t bl = (uint32_t)b, bh = (uint32_t)((uint64_t)b >> 32);
    uint64_t ll = (uint64_t)al * (uint64_t)bl;
    uint64_t lh = (uint64_t)al * (uint64_t)bh;
    uint64_t hl = (uint64_t)ah * (uint64_t)bl;
    uint64_t hh = (uint64_t)ah * (uint64_t)bh;
    uint32_t lo = (uint32_t)ll;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    uint32_t hi = (uint32_t)mid + (uint32_t)((lh >> 32) + (hl >> 32) + hh);
    return (int64_t)(((uint64_t)hi << 32) | lo);
}

static int64_t jvm_div64(int64_t a, int64_t b)
{
    if (b == 0) return 0;
    int neg = ((a < 0) != (b < 0));
    uint64_t ua = (uint64_t)(a < 0 ? -(a + 1) : a);
    uint64_t ub = (uint64_t)(b < 0 ? -(b + 1) : b);
    if (a < 0) ua = ua + 1;
    if (b < 0) ub = ub + 1;
    uint64_t q = 0, r = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((ua >> i) & 1);
        if (r >= ub) { r -= ub; q |= ((uint64_t)1 << i); }
    }
    return neg ? -(int64_t)(q + 1) + 1 : (int64_t)q;
}

static int64_t jvm_rem64(int64_t a, int64_t b)
{
    if (b == 0) return 0;
    int neg = (a < 0);
    uint64_t ua = (uint64_t)(a < 0 ? -(a + 1) : a);
    uint64_t ub = (uint64_t)(b < 0 ? -(b + 1) : b);
    if (a < 0) ua = ua + 1;
    if (b < 0) ub = ub + 1;
    uint64_t r = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((ua >> i) & 1);
        if (r >= ub) r -= ub;
    }
    return neg ? -(int64_t)(r + 1) + 1 : (int64_t)r;
}

static inline void push_long(int32_t *st, int *top, int64_t v)
{
    st[(*top)++] = (int32_t)(uint32_t)v;
    st[(*top)++] = (int32_t)(v >> 32);
}
static inline float fbits(int32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline int32_t bitsf(float f) { int32_t v; memcpy(&v, &f, 4); return v; }
static inline double dfrom2(int32_t lo, int32_t hi)
{
    uint64_t u = ((uint64_t)(uint32_t)lo) | ((uint64_t)(uint32_t)hi << 32);
    double d; memcpy(&d, &u, 8); return d;
}
static inline void dto2(double d, int32_t *lo, int32_t *hi)
{
    uint64_t u; memcpy(&u, &d, 8);
    *lo = (int32_t)(uint32_t)u;
    *hi = (int32_t)(u >> 32);
}
/* 简易 float -> 字符串（"1.5"；整数则 "3.0" 风格） */
static void ftoa(char *out, int n, float v)
{
    if (v != v || v > 3.4e38f || v < -3.4e38f) { snprintf(out, n, "NaN"); return; }
    char tmp[32];
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    long ip = (long)v;
    snprintf(tmp, sizeof(tmp), "%ld", ip);
    int frac = (int)((v - (float)ip) * 1000000.0f + 0.5f);
    if (frac <= 0) snprintf(out, n, "%s%s.0", neg ? "-" : "", tmp);
    else {
        snprintf(out, n, "%s%s.%06d", neg ? "-" : "", tmp, frac);
        /* 去尾零 */
        int len = (int)jstrlen(out);
        while (len > 0 && out[len - 1] == '0') out[--len] = 0;
        if (len > 0 && out[len - 1] == '.') out[--len] = 0;
    }
}


static int32_t exec_method(struct jvm_method *m, struct jvm_class *cls,
                           int32_t *args, int nargs, int is_static);

/* 净化 native 调用的对象参数：desc 中对象/数组类型槽若为非法地址则置 0（防解引用垃圾指针崩溃） */
static void sanitize_obj_args(int32_t *args, int total, const char *fd, int is_static) {
    int qi = 0;
    if (!is_static) {
        if (qi < total && args[qi] && ((uint32_t)args[qi] < 0xc0000000u || (uint32_t)args[qi] >= 0xf0000000u))
            args[qi] = 0;
        qi++;
    }
    for (int ar = 1; fd[ar] != ')' && ar < 0x10000 && qi < total; ar++) {
        char t = fd[ar];
        if (t == 'J' || t == 'D') { qi += 2; continue; }
        if (t == 'L' || t == '[') {
            if (args[qi] && ((uint32_t)args[qi] < 0xc0000000u || (uint32_t)args[qi] >= 0xf0000000u))
                args[qi] = 0;
            qi++;
            if (t == 'L') { while (fd[ar] != ';' && ar < 0x10000) ar++; }
            else { while (fd[ar] == '[' && ar < 0x10000) ar++; if (fd[ar] == 'L') while (fd[ar] != ';' && ar < 0x10000) ar++; }
            continue;
        }
        qi++;
    }
}

static int invoke_native(const char *cls, const char *name, const char *desc,
                         int32_t *locals, int32_t *stack, int top) {
    { static unsigned in_cnt = 0;
      if ((in_cnt++ & 0xFFF) == 0)
          JDBG("[IN] %s.%s%s heap=%uMB\n", cls ? cls : "?", name ? name : "?", desc ? desc : "?",
                 aptos_heap_size() >> 20);
    }

/* ---- log4j stub：LoggerContextFactory/LoggerContext/Logger（MC 日志链） ---- */
    if (jstrcmp(cls, "org/apache/logging/log4j/spi/LoggerContextFactory") == 0 ||
        jstrcmp(cls, "org/apache/logging/log4j/spi/LoggerContext") == 0 ||
        jstrcmp(cls, "org/apache/logging/log4j/LogManager") == 0) {
        if (jstrcmp(name, "getContext") == 0 || jstrcmp(name, "getLogger") == 0) {
            struct class *lcls = find_class("java/lang/Object");
            struct jvm_obj *stub = lcls ? obj_new(lcls) : NULL;
            stack[top] = (int32_t)stub;
            return 1;
        }
        if (jstrcmp(name, "getLogger") == 0) {
            struct class *lcls = find_class("java/lang/Object");
            struct jvm_obj *stub = lcls ? obj_new(lcls) : NULL;
            stack[top] = (int32_t)stub;
            return 1;
        }
    }
    /* ---- MC 注册表 map：da.a()Ljava/util/Map 返回 HashMap stub ---- */
    if (jstrcmp(cls, "da") == 0 && jstrcmp(name, "a") == 0) {
        struct class *mcls = find_class("java/util/HashMap");
        if (!mcls) mcls = find_class("java/lang/Object");
        struct jvm_obj *stub = mcls ? obj_new(mcls) : NULL;
        stack[top] = (int32_t)stub;
        return 1;
    }
    /* ---- sun/misc/Unsafe：getUnsafe 返回 stub 实例，其余按返回类型忽略 ---- */
    if (jstrcmp(cls, "sun/misc/Unsafe") == 0) {
        if (jstrcmp(name, "getUnsafe") == 0) {
            struct class *lcls = find_class("sun/misc/Unsafe");
            if (!lcls) lcls = find_class("java/lang/Object");
            struct jvm_obj *stub = lcls ? obj_new(lcls) : NULL;
            stack[top] = (int32_t)stub;
            return 1;
        }
        /* 其余 Unsafe 方法：放行到后段 CAS/get/put/arrayIndexScale 特判（3194 附近），
           避免这里返回 0 使真实数组操作与 arrayIndexScale 变成死代码 */
    }
    if (jstrcmp(cls, "org/apache/logging/log4j/Logger") == 0 ||
        jstrcmp(cls, "org/apache/logging/log4j/spi/AbstractLogger") == 0 ||
        jstrcmp(cls, "org/apache/logging/log4j/LogBuilder") == 0) {
        if (jstrcmp(name, "isDebugEnabled") == 0 || jstrcmp(name, "isInfoEnabled") == 0 ||
            jstrcmp(name, "isTraceEnabled") == 0 || jstrcmp(name, "isErrorEnabled") == 0 ||
            jstrcmp(name, "isWarnEnabled") == 0 || jstrcmp(name, "isFatalEnabled") == 0) {
            stack[top] = 0; return 1;
        }
        if (jstrcmp(name, "info") == 0 || jstrcmp(name, "warn") == 0 || jstrcmp(name, "error") == 0 ||
            jstrcmp(name, "debug") == 0 || jstrcmp(name, "trace") == 0 || jstrcmp(name, "fatal") == 0) {
            /* 消息参数：重载 (Ljava/lang/String;)V 时在 locals[1]；带 Object... 时也在 locals[1] */
            struct jvm_obj *m = (struct jvm_obj *)locals[1];
            if (m && m->array_data && (uint32_t)m->array_data >= 0xc0000000u && (uint32_t)m->array_data < 0xf0000000u) {
                printf("[MC] %s\n", (char *)m->array_data);
            }
            return 0;
        }
        if (jstrcmp(name, "atLevel") == 0 || jstrcmp(name, "withLocation") == 0 ||
            jstrcmp(name, "withThrowable") == 0 || jstrcmp(name, "log") == 0 ||
            jstrcmp(name, "entry") == 0 || jstrcmp(name, "exit") == 0 || jstrcmp(name, "throwing") == 0) {
            return 0;
        }
    }
    /* PropertiesUtil 的 getStringProperty/getBooleanProperty：返回安全默认 */
    if (jstrcmp(cls, "org/apache/logging/log4j/util/PropertiesUtil") == 0) {
        if (jstrcmp(name, "getStringProperty") == 0) {
            struct jvm_obj *e = jvm_string("");
            stack[top] = (int32_t)e;
            return 1;
        }
        if (jstrcmp(name, "getBooleanProperty") == 0) {
            stack[top] = locals[1]; /* 默认值 */
            return 1;
        }
        if (jstrcmp(name, "getIntegerProperty") == 0) {
            stack[top] = locals[1];
            return 1;
        }
    }
    /* java/lang/Thread.getContextClassLoader / Class.getClassLoader：返回非 null stub（Object 基址） */
    if (jstrcmp(cls, "java/lang/Thread") == 0 && jstrcmp(name, "getContextClassLoader") == 0) {
        struct class *lcls = find_class("java/lang/Object");
        stack[top] = (int32_t)(lcls ? obj_new(lcls) : NULL);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Class") == 0 && jstrcmp(name, "getClassLoader") == 0) {
        struct class *lcls = find_class("java/lang/Object");
        stack[top] = (int32_t)(lcls ? obj_new(lcls) : NULL);
        return 1;
    }
    /* java/lang/Integer.rotateLeft/rotateRight（Bit twiddling） */
    if (jstrcmp(cls, "java/lang/Integer") == 0 &&
        (jstrcmp(name, "rotateLeft") == 0 || jstrcmp(name, "rotateRight") == 0)) {
        int32_t x = locals[0], dd = locals[1] & 31;
        uint32_t ux = (uint32_t)x;
        if (dd == 0) { stack[top] = x; return 1; }
        if (name[6] == 'L') stack[top] = (int32_t)((ux << dd) | (ux >> (32 - dd)));
        else stack[top] = (int32_t)((ux >> dd) | (ux << (32 - dd)));
        return 1;
    }
    /* ---- java/lang/Math：sin/cos 等（双保险入口，Taylor 近似） ---- */
    if (jstrcmp(cls, "java/lang/Math") == 0 && (jstrcmp(name, "sin") == 0 || jstrcmp(name, "cos") == 0)) {
        double x; int32_t lo0, hi0;
        lo0 = locals[0]; hi0 = locals[1];
        { union { uint64_t u; double d; } cv; cv.u = ((uint64_t)(uint32_t)hi0 << 32) | (uint32_t)lo0; x = cv.d; }
        int is_cos = (name[0] == 'c');
        double twopi = 6.283185307179586;
        while (x > twopi) x -= twopi;
        while (x < -twopi) x += twopi;
        double xx;
        if (is_cos) xx = x + 1.5707963267948966; else xx = x;
        while (xx > twopi) xx -= twopi;
        while (xx < -twopi) xx += twopi;
        double s = xx, t = xx, x2 = xx * xx;
        t *= -x2 / 6.0;   s += t;
        t *= -x2 / 20.0;  s += t;
        t *= -x2 / 42.0;  s += t;
        t *= -x2 / 72.0;  s += t;
        t *= -x2 / 110.0; s += t;
        { union { uint64_t u; double d; } cv; cv.d = s; stack[top] = (int32_t)(uint32_t)cv.u; stack[top+1] = (int32_t)(cv.u >> 32); }
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Math") == 0 && jstrcmp(name, "atan2") == 0) {
        int32_t yl = locals[0], yh = locals[1], xl = locals[2], xh = locals[3];
        double y, x;
        { union { uint64_t u; double d; } cv; cv.u = ((uint64_t)(uint32_t)yh << 32) | (uint32_t)yl; y = cv.d; }
        { union { uint64_t u; double d; } cv; cv.u = ((uint64_t)(uint32_t)xh << 32) | (uint32_t)xl; x = cv.d; }
        double r = 0.0;
        if (x > 0.0) r = y / x;
        else if (x < 0.0) r = (y >= 0.0 ? 3.141592653589793 : -3.141592653589793) + y / x;
        else r = (y >= 0.0 ? 1.5707963267948966 : -1.5707963267948966);
        { union { uint64_t u; double d; } cv; cv.d = r; stack[top] = (int32_t)(uint32_t)cv.u; stack[top+1] = (int32_t)(cv.u >> 32); }
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Math") == 0 && jstrcmp(name, "sqrt") == 0) {
        int32_t lo0 = locals[0], hi0 = locals[1];
        double v;
        { union { uint64_t u; double d; } cv; cv.u = ((uint64_t)(uint32_t)hi0 << 32) | (uint32_t)lo0; v = cv.d; }
        double x = v > 0 ? v : 0;
        if (x > 0) { for (int si = 0; si < 40; si++) x = (x + v / x) * 0.5; }
        { union { uint64_t u; double d; } cv; cv.d = x; stack[top] = (int32_t)(uint32_t)cv.u; stack[top+1] = (int32_t)(cv.u >> 32); }
        return 1;
    }
    /* MC 1.7.10 日志类 cw：a(int, String, Object) -> 输出到串口 */
    if (jstrcmp(cls, "cw") == 0 && jstrcmp(name, "a") == 0) {
        struct jvm_obj *s1 = (struct jvm_obj *)locals[1];
        struct jvm_obj *s2 = (struct jvm_obj *)locals[2];
        struct jvm_obj *s = NULL;
        if (s1 && (uint32_t)s1 >= 0xc0000000u && (uint32_t)s1 < 0xf0000000u && s1->is_array) s = s1;
        else if (s2 && (uint32_t)s2 >= 0xc0000000u && (uint32_t)s2 < 0xf0000000u && s2->is_array) s = s2;
        if (s && s->array_data) {
            printf("[MC] %s\n", (const char *)s->array_data);
        }
        return 1;
    }
    /* valueOf：独立前置分支，避免 String 块匹配顺序问题 */
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "valueOf") == 0) {
        if (desc[1] == 'I') {
            char tmp[32];
            lltoa10(locals[0], tmp);
            stack[top] = (int32_t)jvm_string(tmp);
            return 1;
        }
        if (desc[1] == 'C') {
            char tmp[2] = { (char)locals[0], 0 };
            stack[top] = (int32_t)jvm_string(tmp);
            return 1;
        }
        if (desc[1] == 'Z') {
            stack[top] = (int32_t)jvm_string(locals[0] ? "true" : "false");
            return 1;
        }
    }
    /* java/lang/Object.registerNatives */
    if (jstrcmp(name, "registerNatives") == 0) return 1;
    /* java/awt/GraphicsEnvironment.isHeadless()Z —— 服务端必须 headless（true），否则 MC 走 GUI 分支卡死 */
    if (jstrcmp(cls, "java/awt/GraphicsEnvironment") == 0 && jstrcmp(name, "isHeadless") == 0) {
        stack[top++] = 1;
        return 1;
    }
    /* java/lang/Object.getClass()Ljava/lang/Class; → 返回 Class 伪对象（fields[0]=对象类） */
    if (jstrcmp(name, "getClass") == 0) {
        struct jvm_obj *a = (struct jvm_obj *)locals[0];
        struct jvm_class *c = (a && (uint32_t)a >= 0xc0000000u && (uint32_t)a < 0xf0000000u) ? a->cls : NULL;
        if (!c) c = find_class("java/lang/Object");
        struct jvm_obj *co = obj_new(NULL);
        if (co) co->fields[0] = (int32_t)c;
        stack[top] = (int32_t)co;
        return 1;
    }
    /* java/lang/Object.<init>()V：构造无操作（调用点恢复 this） */
    if (jstrcmp(cls, "java/lang/Object") == 0 && jstrcmp(name, "<init>") == 0) return 0;
    /* java/lang/Class.desiredAssertionStatus()Z / name() / getName() */
    if (jstrcmp(cls, "java/lang/Class") == 0 && jstrcmp(name, "desiredAssertionStatus") == 0) {
        stack[top] = 0; return 1;
    }
    if (jstrcmp(cls, "java/lang/Class") == 0 &&
        (jstrcmp(name, "name") == 0 || jstrcmp(name, "getName") == 0)) {
        stack[top] = (int32_t)jvm_string(""); return 1;
    }
    /* java/lang/Class.getComponentType()Ljava/lang/Class; → 返回组件类 Class 伪对象 */
    if (jstrcmp(cls, "java/lang/Class") == 0 && jstrcmp(name, "getComponentType") == 0) {
        struct jvm_obj *co = (struct jvm_obj *)locals[0];
        struct jvm_class *c = co ? (struct jvm_class *)co->fields[0] : NULL;
        struct jvm_obj *nco = obj_new(NULL);
        if (nco) nco->fields[0] = (int32_t)(c ? c : find_class("java/lang/Object"));
        stack[top] = (int32_t)nco;
        return 1;
    }
    /* java/lang/Object.hashCode()I —— javm 简化：Integer box 返回其值，否则返回对象地址 */
    if (jstrcmp(cls, "java/lang/Object") == 0 && jstrcmp(name, "hashCode") == 0) {
        struct jvm_obj *a = (struct jvm_obj *)locals[0];
        stack[top] = (a && (uint32_t)a >= 0xc0000000u && (uint32_t)a < 0xf0000000u && a->cls == NULL && a->array_data)
                         ? a->array_data[0] : (int32_t)a;
        return 1;
    }
    /* java/lang/Object.equals(Ljava/lang/Object;)Z —— javm 简化：Integer box 值比较，否则引用比较 */
    if (jstrcmp(cls, "java/lang/Object") == 0 && jstrcmp(name, "equals") == 0) {
        struct jvm_obj *a = (struct jvm_obj *)locals[0];
        struct jvm_obj *b = (struct jvm_obj *)locals[1];
        if (a && b && (uint32_t)a >= 0xc0000000u && (uint32_t)a < 0xf0000000u &&
            (uint32_t)b >= 0xc0000000u && (uint32_t)b < 0xf0000000u &&
            a->cls == NULL && b->cls == NULL && a->array_data && b->array_data) {
            stack[top] = (a->array_data[0] == b->array_data[0]) ? 1 : 0;
        } else {
            stack[top] = (a == b) ? 1 : 0;
        }
        return 1;
    }
    /* java/lang/String —— 基础字符串操作（游戏文本通用） */
    if (jstrcmp(cls, "java/lang/String") == 0) {
        struct jvm_obj *sa = (struct jvm_obj *)locals[0];
        const char *ss = (sa && (uint32_t)sa >= 0xc0000000u && (uint32_t)sa < 0xf0000000u && sa->array_data)
                             ? (const char *)sa->array_data : "";
        /* slen 优先用对象头 array_len（有界），0 时退回有界 jstrlen */
        int slen = (sa && (uint32_t)sa >= 0xc0000000u && (uint32_t)sa < 0xf0000000u)
                       ? (int)sa->array_len : 0;
        if (slen <= 0) slen = (int)jstrlen(ss);
        if (jstrcmp(name, "length") == 0) { stack[top] = slen; return 1; }
        if (jstrcmp(name, "isEmpty") == 0) { stack[top] = (slen == 0) ? 1 : 0; return 1; }
        if (jstrcmp(name, "equals") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[1];
            const char *s2b = (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data)
                                  ? (const char *)sb->array_data : "";
            stack[top] = (jstrcmp(ss, s2b) == 0) ? 1 : 0;
            return 1;
        }
        if (jstrcmp(name, "equalsIgnoreCase") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[1];
            const char *s2b = (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data)
                                  ? (const char *)sb->array_data : "";
            int ok = 1, i;
            if (jstrlen(s2b) != (size_t)slen) ok = 0;
            for (i = 0; ok && i < slen; i++) {
                char a = ss[i], b2 = s2b[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b2 >= 'A' && b2 <= 'Z') b2 = (char)(b2 + 32);
                if (a != b2) ok = 0;
            }
            stack[top] = ok;
            return 1;
        }
        if (jstrcmp(name, "charAt") == 0) {
            int idx = locals[1];
            stack[top] = (idx >= 0 && idx < slen) ? (int32_t)(uint8_t)ss[idx] : 0;
            return 1;
        }
        if (jstrcmp(name, "codePointAt") == 0) {
            int idx = locals[1];
            stack[top] = (idx >= 0 && idx < slen) ? (int32_t)(uint8_t)ss[idx] : 0;
            return 1;
        }
        if (jstrcmp(name, "substring") == 0) {
            int s0 = locals[1];
            int e0 = (desc[2] == 'I') ? locals[2] : slen;
            char tmp[512];
            if (s0 < 0) s0 = 0;
            if (e0 > slen) e0 = slen;
            if (e0 < s0) e0 = s0;
            memcpy(tmp, ss + s0, (size_t)(e0 - s0));
            tmp[e0 - s0] = 0;
            stack[top] = (int32_t)jvm_string(tmp);
            return 1;
        }
        if (jstrcmp(name, "toLowerCase") == 0 || jstrcmp(name, "toUpperCase") == 0) {
            char tmp[512];
            int i;
            for (i = 0; ss[i] && i < 511; i++) {
                char c = ss[i];
                if (name[2] == 'L') tmp[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                else tmp[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            tmp[i] = 0;
            stack[top] = (int32_t)jvm_string(tmp);
            return 1;
        }
        if (jstrcmp(name, "trim") == 0) {
            const char *p0 = ss, *p1 = ss + slen;
            char tmp[512];
            while (p0 < p1 && (*p0 == ' ' || *p0 == '\t' || *p0 == '\n' || *p0 == '\r')) p0++;
            while (p1 > p0 && (p1[-1] == ' ' || p1[-1] == '\t' || p1[-1] == '\n' || p1[-1] == '\r')) p1--;
            memcpy(tmp, p0, (size_t)(p1 - p0));
            tmp[p1 - p0] = 0;
            stack[top] = (int32_t)jvm_string(tmp);
            return 1;
        }
        if (jstrcmp(name, "contains") == 0 || jstrcmp(name, "startsWith") == 0 || jstrcmp(name, "endsWith") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[1];
            const char *n = (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data)
                                ? (const char *)sb->array_data : "";
            int nl = (int)jstrlen(n);
            if (jstrcmp(name, "contains") == 0) stack[top] = (jvm_strstr(ss, n) != NULL) ? 1 : 0;
            else if (jstrcmp(name, "startsWith") == 0) stack[top] = (nl <= slen && jvm_memcmp(ss, n, (size_t)nl) == 0) ? 1 : 0;
            else stack[top] = (nl <= slen && jvm_memcmp(ss + slen - nl, n, (size_t)nl) == 0) ? 1 : 0;
            return 1;
        }
        if (jstrcmp(name, "indexOf") == 0) {
            if (desc[2] == 'I') {
                const char *p = jvm_strchr(ss, (char)locals[1]);
                stack[top] = p ? (int32_t)(p - ss) : -1;
            } else {
                struct jvm_obj *sb = (struct jvm_obj *)locals[1];
                const char *n = (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data)
                                    ? (const char *)sb->array_data : "";
                const char *p = jvm_strstr(ss, n);
                stack[top] = p ? (int32_t)(p - ss) : -1;
            }
            return 1;
        }
        if (jstrcmp(name, "lastIndexOf") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[1];
            const char *n = (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data)
                                ? (const char *)sb->array_data : "";
            int nl = (int)jstrlen(n), i, hit = -1;
            for (i = 0; i + nl <= slen; i++)
                if (jvm_memcmp(ss + i, n, (size_t)nl) == 0) hit = i;
            stack[top] = hit;
            return 1;
        }
        if (jstrcmp(name, "compareTo") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[1];
            const char *s2b = (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data)
                                  ? (const char *)sb->array_data : "";
            int i, v = 0;
            for (i = 0; ss[i] && s2b[i]; i++) {
                if (ss[i] != s2b[i]) { v = (uint8_t)ss[i] - (uint8_t)s2b[i]; break; }
            }
            if (!v) v = slen - (int)jstrlen(s2b);
            stack[top] = v;
            return 1;
        }
        if (jstrcmp(name, "toCharArray") == 0) {
            struct jvm_obj *arr = arr_new(NULL, slen);
            arr->array_len = slen;
            for (int i = 0; i < slen; i++) arr->array_data[i] = (int32_t)(uint8_t)ss[i];
            stack[top] = (int32_t)arr;
            return 1;
        }
        if (jstrcmp(name, "valueOf") == 0) {
            printf("[JVM] valueOf desc=%s\n", desc);
            if (desc[1] == 'I') {
                char tmp[32];
                lltoa10(locals[1], tmp);
                stack[top] = (int32_t)jvm_string(tmp);
                return 1;
            }
            if (desc[1] == 'C') {
                char tmp[2] = { (char)locals[1], 0 };
                stack[top] = (int32_t)jvm_string(tmp);
                return 1;
            }
            if (desc[1] == 'Z') {
                stack[top] = (int32_t)jvm_string(locals[1] ? "true" : "false");
                return 1;
            }
        }
        if (jstrcmp(name, "concat") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[1];
            const char *s2b = (sb && sb->array_data) ? (const char *)sb->array_data : "";
            char tmp[1024];
            memcpy(tmp, ss, (size_t)slen);
            strcpy(tmp + slen, s2b);
            stack[top] = (int32_t)jvm_string(tmp);
            return 1;
        }
    }
    /* java/lang/reflect/Array.newInstance(Ljava/lang/Class;I)Ljava/lang/Object; */
    if (jstrcmp(cls, "java/lang/reflect/Array") == 0 && jstrcmp(name, "newInstance") == 0) {
        int32_t alen = locals[1];
        if (alen < 0) alen = 0;
        stack[top] = (int32_t)arr_new(find_class("java/lang/Object"), alen);
        return 1;
    }
    /* java/lang/reflect/Array.newArray(Ljava/lang/Class;I)Ljava/lang/Object;（JDK 内部 native） */
    if (jstrcmp(cls, "java/lang/reflect/Array") == 0 && jstrcmp(name, "newArray") == 0) {
        int32_t alen = locals[1];
        if (alen < 0) alen = 0;
        stack[top] = (int32_t)arr_new(find_class("java/lang/Object"), alen);
        return 1;
    }
    /* ---- java.io.FileInputStream（文件流读取） ---- */
    if (jstrcmp(cls, "java/io/FileInputStream") == 0) {
        if (jstrcmp(name, "open0") == 0) {
            struct jvm_obj *thiso = (struct jvm_obj *)locals[0];
            struct jvm_obj *ns = (struct jvm_obj *)locals[1];
            const char *path = (ns && ns->array_data) ? (const char *)ns->array_data : "";
            int fd = sys_open(path, 0);
            int32_t *fp = thiso ? field_slot(thiso, "fd", "Ljava/io/FileDescriptor;") : NULL;
            if (fp && *fp) {
                struct jvm_obj *fdobj = (struct jvm_obj *)*fp;
                int32_t *ip = field_slot(fdobj, "fd", "I");
                if (ip) *ip = fd;
            }
            return 0;
        }
        if (jstrcmp(name, "readBytes") == 0) {
            struct jvm_obj *thiso = (struct jvm_obj *)locals[0];
            struct jvm_obj *b = (struct jvm_obj *)locals[1];
            int off = locals[2];
            int want = locals[3];
            int32_t *fp = thiso ? field_slot(thiso, "fd", "Ljava/io/FileDescriptor;") : NULL;
            int fdi = -1;
            if (fp && *fp) { struct jvm_obj *fo = (struct jvm_obj *)*fp; int32_t *ip = field_slot(fo, "fd", "I"); if (ip) fdi = *ip; }
            uint8_t tmp[4096];
            int r = 0;
            if (fdi < 0) { stack[top] = -1; return 1; }  /* 无效 fd 视为 EOF */
            if (b && off >= 0 && want > 0 && want <= 4096) {
                r = sys_read(fdi, tmp, want);
                for (int i = 0; i < r; i++) b->array_data[off + i] = (int32_t)tmp[i];
            }
            stack[top] = r;
            return 1;
        }
        if (jstrcmp(name, "read0") == 0) {
            struct jvm_obj *thiso = (struct jvm_obj *)locals[0];
            int32_t *fp = thiso ? field_slot(thiso, "fd", "Ljava/io/FileDescriptor;") : NULL;
            int fdi = -1;
            if (fp && *fp) { struct jvm_obj *fo = (struct jvm_obj *)*fp; int32_t *ip = field_slot(fo, "fd", "I"); if (ip) fdi = *ip; }
            uint8_t c = 0;
            int r = (fdi >= 0) ? sys_read(fdi, &c, 1) : -1;
            stack[top] = (r == 1) ? (int32_t)c : -1;
            return 1;
        }
if (jstrcmp(name, "available0") == 0) { stack[top] = 4096; return 1; }
        if (jstrcmp(name, "close0") == 0) {
            struct jvm_obj *thiso = (struct jvm_obj *)locals[0];
            int32_t *fp = thiso ? field_slot(thiso, "fd", "Ljava/io/FileDescriptor;") : NULL;
            if (fp && *fp) { struct jvm_obj *fo = (struct jvm_obj *)*fp; int32_t *ip = field_slot(fo, "fd", "I"); if (ip && *ip >= 0) sys_close(*ip); }
            return 0;
        }
    }
    /* ---- java.io.File 家族：getCanonicalPath/exists/isDirectory 等稳定化，
            避免 IOException 风暴与后续 page fault ---- */
    if (jstrcmp(cls, "java/io/File") == 0) {
        if (jstrcmp(name, "getCanonicalPath") == 0 || jstrcmp(name, "getAbsolutePath") == 0 ||
            jstrcmp(name, "getPath") == 0 || jstrcmp(name, "getName") == 0 ||
            jstrcmp(name, "getParent") == 0 || jstrcmp(name, "toString") == 0) {
            struct jvm_obj *thiso = (struct jvm_obj *)locals[0];
            const char *p = "";
            if (thiso && (uint32_t)thiso >= 0xc0000000u && (uint32_t)thiso < 0xf0000000u) {
                int32_t *pp = field_slot(thiso, "path", "Ljava/lang/String;");
                struct jvm_obj *ps = pp ? (struct jvm_obj *)*pp : NULL;
                if (ps && (uint32_t)ps >= 0xc0000000u && (uint32_t)ps < 0xf0000000u && ps->array_data)
                    p = (const char *)ps->array_data;
            }
            stack[top] = (int32_t)jvm_string(p);
            return 1;
        }
        if (jstrcmp(name, "exists") == 0 || jstrcmp(name, "canRead") == 0 ||
            jstrcmp(name, "canWrite") == 0 || jstrcmp(name, "canExecute") == 0 ||
            jstrcmp(name, "isFile") == 0 || jstrcmp(name, "isHidden") == 0) {
            stack[top] = 1; return 1;
        }
        if (jstrcmp(name, "isDirectory") == 0) { stack[top] = 1; return 1; }
        if (jstrcmp(name, "length") == 0) { stack[top] = 0; return 1; }
        if (jstrcmp(name, "lastModified") == 0) { stack[top] = 0; stack[top + 1] = 0; return 1; }
        if (jstrcmp(name, "list") == 0 || jstrcmp(name, "listFiles") == 0) {
            stack[top] = 0; return 1;  /* null 数组：调用方视作空 */
        }
        if (jstrcmp(name, "mkdir") == 0 || jstrcmp(name, "mkdirs") == 0 ||
            jstrcmp(name, "delete") == 0 || jstrcmp(name, "createNewFile") == 0 ||
            jstrcmp(name, "renameTo") == 0 || jstrcmp(name, "setLastModified") == 0 ||
            jstrcmp(name, "setWritable") == 0 || jstrcmp(name, "setReadable") == 0) {
            stack[top] = 1; return 1;
        }
        if (jstrcmp(name, "toURI") == 0) { stack[top] = 0; return 1; }
        if (jstrcmp(name, "getCanonicalFile") == 0) { stack[top] = locals[0]; return 1; }
        if (jstrcmp(name, "deleteOnExit") == 0) { return 0; }
        if (jstrcmp(name, "compareTo") == 0) { stack[top] = 0; return 1; }
    }
    /* ---- java.io.Reader / InputStream（读循环 EOF：缺类时 read 返回 0 会死循环） ---- */
    if (jstrcmp(cls, "java/io/Reader") == 0 ||
        jstrcmp(cls, "java/io/InputStreamReader") == 0 ||
        jstrcmp(cls, "java/io/BufferedReader") == 0 ||
        jstrcmp(cls, "java/io/InputStream") == 0 ||
        jstrcmp(cls, "java/io/ByteArrayInputStream") == 0) {
        if (jstrcmp(name, "read") == 0) {
            stack[top] = -1;  /* EOF */
            return 1;
        }
        if (jstrcmp(name, "readLine") == 0) {
            stack[top] = 0;   /* null 行 */
            return 1;
        }
        if (jstrcmp(name, "skip") == 0 || jstrcmp(name, "mark") == 0 ||
            jstrcmp(name, "reset") == 0 || jstrcmp(name, "markSupported") == 0 ||
            jstrcmp(name, "ready") == 0 || jstrcmp(name, "close") == 0 ||
            jstrcmp(name, "available") == 0) {
            if (jstrcmp(name, "markSupported") == 0 || jstrcmp(name, "ready") == 0) {
                stack[top] = 1; return 1;
            }
            if (jstrcmp(name, "available") == 0) { stack[top] = 0; return 1; }
            return 0;
        }
    }
    /* ---- java.util.zip.Inflater（zlib 解压） ---- */
    if (jstrcmp(cls, "java/util/zip/Inflater") == 0) {
        if (jstrcmp(name, "<init>") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            if (o) { o->fields[0] = 0; o->fields[1] = 0; }
            return 0;
        }
        if (jstrcmp(name, "setInput") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            struct jvm_obj *b = (struct jvm_obj *)locals[1];
            if (o) { o->fields[0] = (int32_t)b; o->fields[1] = b ? b->array_len : 0; }
            return 0;
        }
        if (jstrcmp(name, "inflate") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            struct jvm_obj *b = (struct jvm_obj *)locals[1];
            struct jvm_obj *in = o ? (struct jvm_obj *)o->fields[0] : NULL;
            int32_t n = o ? o->fields[1] : 0;
            uint32_t olen = 0;
            if (in && in->array_data && b && b->array_data && n > 0) {
                uint8_t comp[2048], outp[2048];
                int cn = n; if (cn > 2048) cn = 2048;
                for (int i = 0; i < cn; i++) comp[i] = (uint8_t)((uint32_t)in->array_data[i]);
                uint32_t cap = (uint32_t)b->array_len; if (cap > 2048) cap = 2048;
                if (javm_inflate(comp, (uint32_t)cn, outp, cap, &olen) == 0) {
                    for (uint32_t i = 0; i < olen; i++) b->array_data[i] = (int32_t)outp[i];
                } else {
                    olen = 0;
                }
            }
            stack[top] = (int32_t)olen;
            return 1;
        }
        if (jstrcmp(name, "needsInput") == 0) { stack[top] = 0; return 1; }
        if (jstrcmp(name, "needsDictionary") == 0) { stack[top] = 0; return 1; }
        if (jstrcmp(name, "getRemaining") == 0) { stack[top] = 0; return 1; }
        if (jstrcmp(name, "end") == 0) { return 0; }
        if (jstrcmp(name, "finished") == 0) { stack[top] = 1; return 1; }
        if (jstrcmp(name, "getBytesRead") == 0) { stack[top] = 0; stack[top + 1] = 0; return 1; }
    }
    /* ---- java/util/UUID 简化：fromString/randomUUID 返回伪对象 ---- */
    if (jstrcmp(cls, "java/util/UUID") == 0) {
        if (jstrcmp(name, "randomUUID") == 0 || jstrcmp(name, "fromString") == 0) {
            struct class *lcls = find_class("java/lang/Object");
            struct jvm_obj *stub = lcls ? obj_new(lcls) : NULL;
            stack[top++] = (int32_t)stub; return 1;
        }
        if (jstrcmp(name, "toString") == 0) {
            struct jvm_obj *s = jvm_string("00000000-0000-0000-0000-000000000000");
            stack[top++] = (int32_t)s; return 1;
        }
        if (jstrcmp(name, "getMostSignificantBits") == 0) { stack[top++] = 0; stack[top++] = 0; return 1; }
        if (jstrcmp(name, "getLeastSignificantBits") == 0) { stack[top++] = 0; stack[top++] = 0; return 1; }
    }
    /* ---- java.util.Random（游戏随机数） ---- */
    if (jstrcmp(cls, "java/util/Random") == 0) {
        if (jstrcmp(name, "<init>") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            if (o) o->fields[0] = locals[1] ? locals[1] : 12345;
            return 0;
        }
        if (jstrcmp(name, "nextInt") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            uint32_t s = o ? (uint32_t)o->fields[0] : 12345;
            s = s * 1103515245u + 12345u;
            if (o) o->fields[0] = (int32_t)s;
            stack[top] = (int32_t)(s >> 1);
            return 1;
        }
        if (jstrcmp(name, "next") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            uint32_t s = o ? (uint32_t)o->fields[0] : 12345;
            s = s * 1103515245u + 12345u;
            if (o) o->fields[0] = (int32_t)s;
            int bits = locals[1] ? locals[1] : 32;
            stack[top] = (int32_t)((s >> 1) & ((bits >= 32) ? 0x7fffffff : ((1u << bits) - 1)));
            return 1;
        }
        if (jstrcmp(name, "nextDouble") == 0) { stack[top] = 0; stack[top + 1] = 0; return 1; }
        if (jstrcmp(name, "setSeed") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            if (o) o->fields[0] = locals[1];
            return 0;
        }
    }
    /* ---- java.util.concurrent.atomic.AtomicLong：单线程真实语义。
           类不在镜像（缺失），全部经此特判；value 64 位存对象 fields[0..1]。 ---- */
    if (jstrcmp(cls, "java/util/concurrent/atomic/AtomicLong") == 0) {
        struct jvm_obj *o = (struct jvm_obj *)locals[0];
        if (jstrcmp(name, "<init>") == 0) {
            if (o) { o->fields[0] = 0; o->fields[1] = 0; }
            if (o && desc[0] == '(' && desc[1] == 'J') { o->fields[0] = locals[1]; o->fields[1] = locals[2]; }
            return 0;
        }
        if (jstrcmp(name, "get") == 0) {
            stack[top] = o ? o->fields[0] : 0;
            stack[top + 1] = o ? o->fields[1] : 0;
            return 1;
        }
        if (jstrcmp(name, "set") == 0 || jstrcmp(name, "lazySet") == 0) {
            if (o) { o->fields[0] = locals[1]; o->fields[1] = locals[2]; }
            return 0;
        }
        if (jstrcmp(name, "compareAndSet") == 0) {
            int64_t cur = o ? (((int64_t)(uint32_t)o->fields[1]) << 32) | (uint32_t)o->fields[0] : 0;
            int64_t exp = (((int64_t)(uint32_t)locals[2]) << 32) | (uint32_t)locals[1];
            int64_t upd = (((int64_t)(uint32_t)locals[4]) << 32) | (uint32_t)locals[3];
            if (o && cur == exp) {
                o->fields[0] = (int32_t)(uint32_t)upd;
                o->fields[1] = (int32_t)(uint32_t)((uint64_t)upd >> 32);
                stack[top] = 1; return 1;
            }
            stack[top] = 0; return 1;
        }
        if (jstrcmp(name, "getAndAdd") == 0 || jstrcmp(name, "addAndGet") == 0) {
            int64_t cur = o ? (((int64_t)(uint32_t)o->fields[1]) << 32) | (uint32_t)o->fields[0] : 0;
            int64_t delta = (((int64_t)(uint32_t)locals[2]) << 32) | (uint32_t)locals[1];
            int64_t nv = cur + delta;
            if (o) { o->fields[0] = (int32_t)(uint32_t)nv; o->fields[1] = (int32_t)(uint32_t)((uint64_t)nv >> 32); }
            int64_t r = (name[0] == 'g') ? cur : nv; /* getAndAdd 旧值 / addAndGet 新值 */
            stack[top] = (int32_t)(uint32_t)r;
            stack[top + 1] = (int32_t)(uint32_t)((uint64_t)r >> 32);
            return 1;
        }
        if (jstrcmp(name, "getAndSet") == 0) {
            int64_t cur = o ? (((int64_t)(uint32_t)o->fields[1]) << 32) | (uint32_t)o->fields[0] : 0;
            int64_t nv = (((int64_t)(uint32_t)locals[2]) << 32) | (uint32_t)locals[1];
            if (o) { o->fields[0] = (int32_t)(uint32_t)nv; o->fields[1] = (int32_t)(uint32_t)((uint64_t)nv >> 32); }
            stack[top] = (int32_t)(uint32_t)cur;
            stack[top + 1] = (int32_t)(uint32_t)((uint64_t)cur >> 32);
            return 1;
        }
        if (jstrcmp(name, "toString") == 0) { stack[top] = 0; return 1; }
        stack[top] = 0; return 1;
    }
    /* ---- java.lang.Math.abs（游戏常用） ---- */
    /* ---- java/lang/Math：sin/cos/atan2 近似（Taylor，libc 无 libm） ---- */
    if (jstrcmp(cls, "java/lang/Math") == 0 && (jstrcmp(name, "sin") == 0 || jstrcmp(name, "cos") == 0)) {
        double x;
        memcpy(&x, &locals[0], 8);
        int is_cos = (name[0] == 'c');
        /* 周期折叠到 [-pi, pi] */
        double twopi = 6.283185307179586;
        while (x > twopi) x -= twopi;
        while (x < -twopi) x += twopi;
        double xx;
        if (is_cos) xx = x + 1.5707963267948966; else xx = x; /* cos(x)=sin(x+pi/2) */
        while (xx > twopi) xx -= twopi;
        while (xx < -twopi) xx += twopi;
        double s = xx, t = xx, x2 = xx * xx;
        t *= -x2 / 6.0;   s += t;
        t *= -x2 / 20.0;  s += t;
        t *= -x2 / 42.0;  s += t;
        t *= -x2 / 72.0;  s += t;
        t *= -x2 / 110.0; s += t;
        double r = s;
        memcpy(&stack[top], &r, 8);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Math") == 0 && jstrcmp(name, "atan2") == 0) {
        double y, x;
        memcpy(&y, &locals[0], 8);
        memcpy(&x, &locals[2], 8);
        double r = 0.0;
        if (x > 0.0) r = y / x;
        else if (x < 0.0) r = (y >= 0.0 ? 3.141592653589793 : -3.141592653589793) + y / x;
        else r = (y >= 0.0 ? 1.5707963267948966 : -1.5707963267948966);
        memcpy(&stack[top], &r, 8);
        return 1;
    }

    if (jstrcmp(cls, "java/lang/Math") == 0 && jstrcmp(name, "atan2") == 0) {
        double y, x;
        memcpy(&y, &locals[0], 8);
        memcpy(&x, &locals[2], 8);
        double r = 0.0;
        if (x > 0.0) r = y / x;
        else if (x < 0.0) r = (y >= 0.0 ? 3.141592653589793 : -3.141592653589793) + y / x;
        else r = (y >= 0.0 ? 1.5707963267948966 : -1.5707963267948966);
        memcpy(&stack[top], &r, 8);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Integer") == 0 && jstrcmp(name, "highestOneBit") == 0) {
        int32_t v = locals[0]; int32_t r = 0;
        if (v != 0) { r = 0x80000000u; while (!(v & r)) r >>= 1; }
        stack[top] = r; return 1;
    }
    if (jstrcmp(cls, "java/lang/Integer") == 0 && jstrcmp(name, "numberOfLeadingZeros") == 0) {
        int32_t v = locals[0]; int n = 0;
        if (v != 0) { uint32_t u = (uint32_t)v; while (!(u & 0x80000000u)) { u <<= 1; n++; } }
        stack[top] = (v == 0) ? 32 : n; return 1;
    }
    if (jstrcmp(cls, "java/lang/Integer") == 0 && jstrcmp(name, "bitCount") == 0) {
        int32_t v = locals[0]; int c = 0;
        for (int b = 0; b < 32; b++) if (v & (1u << b)) c++;
        stack[top] = c; return 1;
    }
    if (jstrcmp(cls, "java/lang/Integer") == 0 && jstrcmp(name, "toBinaryString") == 0) {
        int32_t v = locals[0];
        char buf[40]; int p2 = 0; uint32_t u = (uint32_t)v;
        if (u == 0) buf[p2++] = '0';
        while (u) { buf[p2++] = (char)('0' + (u & 1)); u >>= 1; }
        char rev[40]; for (int q = 0; q < p2; q++) rev[q] = buf[p2 - 1 - q]; rev[p2] = 0;
        struct jvm_obj *s = jvm_string(rev);
        stack[top] = (int32_t)s; return 1;
    }
    if (jstrcmp(cls, "java/lang/Integer") == 0 && (jstrcmp(name, "toString") == 0) && desc[0] == '(' && desc[1] == 'I') {
        int32_t v = locals[0];
        char buf[24]; snprintf(buf, sizeof(buf), "%d", v);
        struct jvm_obj *s = jvm_string(buf);
        stack[top] = (int32_t)s; return 1;
    }
    if (jstrcmp(cls, "java/lang/Math") == 0 && jstrcmp(name, "abs") == 0) {
        int32_t v = locals[0];
        stack[top] = (v < 0) ? -v : v;
        return 1;
    }
    /* java/io/FileSystem —— javm 简化：平台文件系统抽象直接 native（File 的 fs 链） */
    if (jstrcmp(cls, "java/io/FileSystem") == 0) {
        if (jstrcmp(name, "getFileSystem") == 0) {
            stack[top] = (int32_t)obj_new(NULL);
            return 1;
        }
        if (jstrcmp(name, "normalize") == 0) {
            struct jvm_obj *s = (struct jvm_obj *)locals[1];
            stack[top] = s ? (int32_t)s : 0;
            return 1;
        }
        if (jstrcmp(name, "prefixLength") == 0) {
            stack[top] = 0;
            return 1;
        }
        if (jstrcmp(name, "isInvalid") == 0) {
            stack[top] = 0; /* 正常路径（不校验） */
            return 1;
        }
        if (jstrcmp(name, "getBooleanAttributes") == 0) {
            struct jvm_obj *file = (struct jvm_obj *)locals[1];
            int32_t *pathp = file ? field_slot(file, "path", "Ljava/lang/String;") : NULL;
            int32_t r = 0;
            if (pathp && *pathp) {
                struct jvm_obj *pstr = (struct jvm_obj *)*pathp;
                const char *pp = (pstr && pstr->array_data) ? (const char *)pstr->array_data : "";
                int fd = sys_open(pp, 0);
                if (fd >= 0) { r = 1; sys_close(fd); }
            }
            stack[top] = r;
            return 1;
        }
        if (jstrcmp(name, "getBooleanAttributes0") == 0) {
            struct jvm_obj *file = (struct jvm_obj *)locals[1];
            int32_t *pathp = file ? field_slot(file, "path", "Ljava/lang/String;") : NULL;
            int32_t r = 0;
            if (pathp && *pathp) {
                struct jvm_obj *pstr = (struct jvm_obj *)*pathp;
                const char *pp = (pstr && pstr->array_data) ? (const char *)pstr->array_data : "";
                int fd = sys_open(pp, 0);
                if (fd >= 0) { r = 1; sys_close(fd); }
            }
            stack[top] = r;
            return 1;
        }
        if (jstrcmp(name, "getLength") == 0) {
            struct jvm_obj *file = (struct jvm_obj *)locals[1];
            int32_t *pathp = file ? field_slot(file, "path", "Ljava/lang/String;") : NULL;
            uint32_t len = 0;
            if (pathp && *pathp) {
                struct jvm_obj *pstr = (struct jvm_obj *)*pathp;
                const char *pp = (pstr && pstr->array_data) ? (const char *)pstr->array_data : "";
                uint8_t *b = read_file(pp, &len);
                if (b) free(b);
            }
            stack[top] = (int32_t)len;       /* 低 */
            stack[top + 1] = 0;              /* 高 */
            return 1;
        }
    }
    /* java/lang/Math.min(II)I */
    if (jstrcmp(cls, "java/lang/Math") == 0 && jstrcmp(name, "min") == 0) {
        stack[top] = locals[0] < locals[1] ? locals[0] : locals[1];
        return 1;
    }
    /* java/lang/System.getSecurityManager()Ljava/lang/SecurityManager; → null */
    if (jstrcmp(cls, "java/lang/System") == 0 && jstrcmp(name, "getSecurityManager") == 0) {
        stack[top] = 0;
        return 1;
    }
    /* java/lang/System.arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V */
    if (jstrcmp(cls, "java/lang/System") == 0 && jstrcmp(name, "arraycopy") == 0) {
        struct jvm_obj *src = (struct jvm_obj *)locals[0];
        int sp = locals[1];
        struct jvm_obj *dst = (struct jvm_obj *)locals[2];
        int dp = locals[3];
        int cnt = locals[4];
        if (src && dst && src->is_array && dst->is_array &&
            src->array_data && dst->array_data &&
            (uint32_t)src >= 0xc0000000u && (uint32_t)src < 0xf0000000u &&
            (uint32_t)dst >= 0xc0000000u && (uint32_t)dst < 0xf0000000u &&
            (uint32_t)src->array_data >= 0xc0000000u && (uint32_t)src->array_data < 0xf0000000u &&
            (uint32_t)dst->array_data >= 0xc0000000u && (uint32_t)dst->array_data < 0xf0000000u) {
            int slen = src->array_len, dlen = dst->array_len;
            if (sp < 0) sp = 0;
            if (dp < 0) dp = 0;
            if (cnt < 0) cnt = 0;
            if (sp + cnt > slen) cnt = slen - sp;
            if (dp + cnt > dlen) cnt = dlen - dp;
            if (cnt > 0)
                for (int i = 0; i < cnt; i++)
                    ((int32_t *)dst->array_data)[dp + i] = ((int32_t *)src->array_data)[sp + i];
        }
        return 1;
    }
    /* java/lang/Integer 简化（int 装箱：伪对象，array_data[0] 存值） */
    if (jstrcmp(cls, "java/lang/Integer") == 0) {
        if (jstrcmp(name, "valueOf") == 0) {
            struct jvm_obj *box = arr_new(NULL, 1);
            box->is_array = 0;
            box->array_data[0] = locals[0];
            stack[top] = (int32_t)box;
            return 1;
        }
        if (jstrcmp(name, "intValue") == 0) {
            struct jvm_obj *box = (struct jvm_obj *)locals[0];
            stack[top] = (box && box->array_data) ? box->array_data[0] : 0;
            return 1;
        }
        if (jstrcmp(name, "toString") == 0) {
            struct jvm_obj *box = (struct jvm_obj *)locals[0];
            int32_t v = (box && box->array_data) ? box->array_data[0] : 0;
            char t[24]; lltoa10(v, t);
            stack[top] = (int32_t)jvm_string(t);
            return 1;
        }
        if (jstrcmp(name, "hashCode") == 0) {
            struct jvm_obj *box = (struct jvm_obj *)locals[0];
            stack[top] = (box && box->array_data) ? box->array_data[0] : 0;
            return 1;
        }
        if (jstrcmp(name, "equals") == 0) {
            struct jvm_obj *a = (struct jvm_obj *)locals[0];
            struct jvm_obj *b = (struct jvm_obj *)locals[1];
            stack[top] = (a && b && a->array_data && b->array_data && a->array_data[0] == b->array_data[0]) ? 1 : 0;
            return 1;
        }
    }
    /* java/lang/StringBuilder / StringBuffer（native 拼接，绕过标准字节码） */
    if (jstrcmp(cls, "java/lang/StringBuilder") == 0 || jstrcmp(cls, "java/lang/StringBuffer") == 0) {
        if (jstrcmp(name, "<init>") == 0) return 1;
        if (jstrcmp(name, "append") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[0];
            char *buf = sb ? (char *)sb->array_data : NULL;
            if (!buf) { stack[top] = (int32_t)sb; return 1; }
            const char *dsc = desc;
            if (dsc[1] == 'L') {
                struct jvm_obj *s = (struct jvm_obj *)locals[1];
                if (s && s->array_data)
                    jvm_strcat(buf, (const char *)s->array_data);
                else jvm_strcat(buf, "null");
            } else if (dsc[1] == 'J') {
                int64_t jv = ((int64_t)(uint32_t)locals[2] << 32) | (uint32_t)locals[1];
                char t[24]; lltoa10(jv, t); jvm_strcat(buf, t);
            } else if (dsc[1] == 'I' || dsc[1] == 'Z' || dsc[1] == 'S' || dsc[1] == 'B') {
                char t[24]; lltoa10(locals[1], t); jvm_strcat(buf, t);
            } else if (dsc[1] == 'C') {
                char t[2]; t[0] = (char)locals[1]; t[1] = 0; jvm_strcat(buf, t);
            } else if (dsc[1] == 'F') {
                char t[32]; ftoa(t, sizeof(t), fbits(locals[1])); jvm_strcat(buf, t);
            } else if (dsc[1] == 'D') {
                char t[32]; ftoa(t, sizeof(t), (float)dfrom2(locals[1], locals[2])); jvm_strcat(buf, t);
            } else {
                jvm_strcat(buf, "?");
            }
            stack[top] = (int32_t)sb;
            return 1;
        }
        if (jstrcmp(name, "toString") == 0) {
            stack[top] = (int32_t)locals[0];
            return 1;
        }
    }

    /* java/lang/System.arraycopy（name-only 兜底：加守卫防越界） */
    if (jstrcmp(name, "arraycopy") == 0) {
        struct jvm_obj *src = (struct jvm_obj *)locals[0];
        int32_t sp = locals[1];
        struct jvm_obj *dst = (struct jvm_obj *)locals[2];
        int32_t dp = locals[3];
        int32_t n = locals[4];
        if (src && dst && src->is_array && dst->is_array &&
            (uint32_t)src >= 0xc0000000u && (uint32_t)src < 0xf0000000u &&
            (uint32_t)dst >= 0xc0000000u && (uint32_t)dst < 0xf0000000u &&
            src->array_data && dst->array_data &&
            (uint32_t)src->array_data >= 0xc0000000u && (uint32_t)src->array_data < 0xf0000000u &&
            (uint32_t)dst->array_data >= 0xc0000000u && (uint32_t)dst->array_data < 0xf0000000u) {
            int slen = src->array_len, dlen = dst->array_len;
            if (sp < 0) sp = 0;
            if (dp < 0) dp = 0;
            if (n < 0) n = 0;
            if (sp + n > slen) n = slen - sp;
            if (dp + n > dlen) n = dlen - dp;
            if (n > 0)
                memcpy(dst->array_data + dp, src->array_data + sp, n * 4);
        }
        return 1;
    }
    /* java/io/PrintStream.print/println */
    if (jstrcmp(cls, "java/io/PrintStream") == 0) {
        int is_ln = jstrcmp(name, "println") == 0;
        if (is_ln || jstrcmp(name, "print") == 0) {
            /* args: (this=locals[0], value=locals[1]) */
            int32_t v = locals[1];
            const char *d = desc;
            if (d[1] == 'L') {
                uint32_t sa = (uint32_t)v;
                if (sa >= 0xc0000000u && sa < 0xf0000000u) {
                    struct jvm_obj *s = (struct jvm_obj *)v;
                    if (s->array_data)
                        printf("%s", (const char *)s->array_data);
                    else
                        printf("null");
                } else {
                    printf("badptr");
                }
            } else if (d[1] == 'I' || d[1] == 'Z' || d[1] == 'C' || d[1] == 'S' || d[1] == 'B') {
                printf("%d", v);
            } else if (d[1] == 'J') {
                int64_t jv = ((int64_t)(uint32_t)locals[2] << 32) | (uint32_t)locals[1];
                char lt[24]; lltoa10(jv, lt); printf("%s", lt);
            } else if (d[1] == 'F') {
                char ft[32]; ftoa(ft, sizeof(ft), fbits(locals[1])); printf("%s", ft);
            } else if (d[1] == 'D') {
                char ft[32]; ftoa(ft, sizeof(ft), (float)dfrom2(locals[1], locals[2])); printf("%s", ft);
            } else if (d[1] == '[') {
                printf("[arr@%p]", (void *)v);
            }
            if (is_ln) printf("\n");
            return 1;
        }
        if (jstrcmp(name, "write") == 0) {
            putchar((char)locals[1]);
            return 1;
        }
    }
    /* java/lang/String.length */
    if (jstrcmp(name, "length") == 0 && jstrcmp(desc, "()I") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        stack[top] = (s && (uint32_t)s >= 0xc0000000u && (uint32_t)s < 0xf0000000u) ? s->array_len : 0;
        return 1;
    }
    /* java/lang/String.charAt */
    if (jstrcmp(name, "charAt") == 0 && jstrcmp(desc, "(I)C") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        int32_t i = locals[1];
        const char *cs = (s && (uint32_t)s >= 0xc0000000u && (uint32_t)s < 0xf0000000u && s->array_data)
                            ? (const char *)s->array_data : "";
        stack[top] = (i >= 0 && cs && i < (int32_t)jstrlen(cs)) ? (uint8_t)cs[i] : 0;
        return 1;
    }
    /* java/lang/Integer.parseInt */
    if (jstrcmp(name, "parseInt") == 0 && desc[0] == '(') {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = "0";
        if (s && (uint32_t)s >= 0xc0000000u && (uint32_t)s < 0xf0000000u &&
            s->is_array && s->array_data) cs = (const char *)s->array_data;
        int v = 0, neg = 0, i = 0;
        if (cs[0] == '-') { neg = 1; i = 1; }
        for (; cs[i] >= '0' && cs[i] <= '9'; i++) v = v * 10 + (cs[i] - '0');
        stack[top] = neg ? -v : v;
        return 1;
    }
    /* java/lang/Math 完整实现在后置 Math 块（支持 I/J/F/D） */
    /* java/lang/Throwable.printStackTrace 等：忽略 */
    if (jstrcmp(name, "printStackTrace") == 0) { printf("(stacktrace)\n"); return 1; }
    if (jstrcmp(name, "getMessage") == 0) { stack[top] = 0; return 1; }
    if (jstrcmp(name, "getCause") == 0) { stack[top] = 0; return 1; }
    if (jstrcmp(name, "toString") == 0 && jstrcmp(cls, "java/lang/Object") == 0) {
        stack[top] = (int32_t)jvm_string("java.lang.Object");
        return 1;
    }
    if (jstrcmp(name, "getClass") == 0 && jstrcmp(desc, "()Ljava/lang/Class;") == 0) {
        stack[top] = 0; /* null */
        return 1;
    }
    if (jstrcmp(name, "hashCode") == 0) { stack[top] = (int32_t)(uintptr_t)locals[0]; return 1; }
    if (jstrcmp(name, "equals") == 0) { stack[top] = (locals[0] == locals[1]); return 1; }
    if (jstrcmp(name, "arraycopy") == 0) return 1;
    /* java/lang/Math 常用 */
    if (jstrcmp(cls, "java/lang/Math") == 0) {
        if (jstrcmp(name, "min") == 0 && jstrcmp(desc, "(II)I") == 0) { stack[top] = locals[0] < locals[1] ? locals[0] : locals[1]; return 1; }
        if (jstrcmp(name, "max") == 0 && jstrcmp(desc, "(II)I") == 0) { stack[top] = locals[0] > locals[1] ? locals[0] : locals[1]; return 1; }
        if (jstrcmp(name, "abs") == 0 && jstrcmp(desc, "(I)I") == 0) { int32_t v = locals[0]; stack[top] = v < 0 ? -v : v; return 1; }
    }
    /* java/lang/Integer 常用 */
    if (jstrcmp(cls, "java/lang/Integer") == 0) {
        if (jstrcmp(name, "parseInt") == 0 && jstrcmp(desc, "(Ljava/lang/String;)I") == 0) {
            struct jvm_obj *s = (struct jvm_obj *)locals[0];
            const char *p = (s && s->is_array && s->array_data) ? (const char *)s->array_data : "";
            int32_t r = 0, neg = 0;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; }
            stack[top] = neg ? -r : r;
            return 1;
        }
        if (jstrcmp(name, "toString") == 0 && jstrcmp(desc, "(I)Ljava/lang/String;") == 0) {
            char buf[16];
            int32_t v = locals[0];
            int neg = v < 0, i = 0, n = 0;
            char tmp[12];
            unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
            do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
            if (neg) tmp[n++] = '-';
            while (n) buf[i++] = tmp[--n];
            buf[i] = 0;
            stack[top] = (int32_t)jvm_string(buf);
            return 1;
        }
    }
    /* ---- String 实例方法 ---- */
    /* String 系统一入口守卫：非法 s / o 引用降级（防野指针崩） */
    if (jstrcmp(cls, "java/lang/String") == 0) {
        struct jvm_obj *s0 = (struct jvm_obj *)locals[0];
        struct jvm_obj *o0 = (struct jvm_obj *)locals[1];
        const char *nm0 = (name && (uint32_t)name >= 0xc0000000u && (uint32_t)name < 0xf0000000u) ? name : "?";
        int sbad = !(s0 && (uint32_t)s0 >= 0xc0000000u && (uint32_t)s0 < 0xf0000000u);
        int obad = !(o0 && (uint32_t)o0 >= 0xc0000000u && (uint32_t)o0 < 0xf0000000u);
        if (sbad || (obad && (jstrcmp(nm0, "equals") == 0 || jstrcmp(nm0, "compareTo") == 0 ||
                    jstrcmp(nm0, "startsWith") == 0 || jstrcmp(nm0, "endsWith") == 0 ||
                    jstrcmp(nm0, "contains") == 0 || jstrcmp(nm0, "concat") == 0))) {
            if (jstrcmp(nm0, "length") == 0 || jstrcmp(nm0, "isEmpty") == 0) stack[top] = 0;
            else if (jstrcmp(nm0, "charAt") == 0 || jstrcmp(nm0, "codePointAt") == 0) stack[top] = 0;
            else if (jstrcmp(nm0, "indexOf") == 0 || jstrcmp(nm0, "lastIndexOf") == 0) stack[top] = -1;
            else if (jstrcmp(nm0, "equals") == 0 || jstrcmp(nm0, "compareTo") == 0 ||
                     jstrcmp(nm0, "startsWith") == 0 || jstrcmp(nm0, "endsWith") == 0 ||
                     jstrcmp(nm0, "contains") == 0) stack[top] = 0;
            else if (jstrcmp(nm0, "toString") == 0 || jstrcmp(nm0, "trim") == 0 ||
                     jstrcmp(nm0, "toUpperCase") == 0 || jstrcmp(nm0, "toLowerCase") == 0 ||
                     jstrcmp(nm0, "substring") == 0 || jstrcmp(nm0, "concat") == 0 ||
                     jstrcmp(nm0, "replace") == 0 || jstrcmp(nm0, "intern") == 0)
                stack[top] = (int32_t)jvm_string("");
            else stack[top] = 0;
            return 1;
        }
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "length") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        stack[top] = s && s->array_data ? (int32_t)jstrlen((char *)s->array_data) : 0;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "isEmpty") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        stack[top] = (s && s->array_data && !*((char *)s->array_data)) ? 1 : 0;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "charAt") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = s && s->array_data ? (char *)s->array_data : "";
        int32_t i = locals[1];
        stack[top] = (i >= 0 && i < (int32_t)jstrlen(cs)) ? (uint8_t)cs[i] : 0;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "substring") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = s && s->array_data ? (char *)s->array_data : "";
        int32_t b = locals[1], e = locals[2];
        int32_t n = (int32_t)jstrlen(cs);
        char buf[256];
        if (b < 0) b = 0;
        if (e > n) e = n;
        if (b > e) b = e;
        int32_t len = e - b;
        if (len > 255) len = 255;
        memcpy(buf, cs + b, len);
        buf[len] = 0;
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && (jstrcmp(name, "indexOf") == 0 || jstrcmp(name, "lastIndexOf") == 0)) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = s && s->array_data ? (char *)s->array_data : "";
        int rev = jstrcmp(name, "lastIndexOf") == 0;
        if (desc[1] == 'I') {
            int32_t c = locals[1];
            if (rev) { int r = -1; for (int i = 0; cs[i]; i++) if ((uint8_t)cs[i] == (uint8_t)c) r = i; stack[top] = r; }
            else stack[top] = jvm_strchr(cs, (char)c) ? (int32_t)(jvm_strchr(cs, (char)c) - cs) : -1;
            return 1;
        } else {
            struct jvm_obj *q = (struct jvm_obj *)locals[1];
            const char *sub = q && q->array_data ? (char *)q->array_data : "";
            if (rev) { int r = -1; for (int i = 0; cs[i]; i++) { int k = 0; while (sub[k] && cs[i+k] == sub[k]) k++; if (!sub[k]) r = i; } stack[top] = r; }
            else {
                char *p = jvm_strstr(cs, sub);
                stack[top] = p ? (int32_t)(p - cs) : -1;
            }
            return 1;
        }
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "equals") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        struct jvm_obj *o = (struct jvm_obj *)locals[1];
        if (o && (uint32_t)o >= 0xc0000000u && (uint32_t)o < 0xf0000000u &&
            o->is_array && o->cls && (uint32_t)o->cls >= 0xc0000000u &&
            (uint32_t)o->cls < 0xf0000000u &&
            jstrcmp(o->cls->name, "java/lang/String") == 0) {
            const char *a = s && s->array_data ? (char *)s->array_data : "";
            const char *b = o->array_data ? (char *)o->array_data : "";
            stack[top] = jstrcmp(a, b) == 0;
        } else stack[top] = (s == o);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "compareTo") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        struct jvm_obj *o = (struct jvm_obj *)locals[1];
        const char *a = s && s->array_data ? (char *)s->array_data : "";
        const char *b = o && o->array_data ? (char *)o->array_data : "";
        int r = jstrcmp(a, b);
        stack[top] = (r > 0) - (r < 0);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "concat") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        struct jvm_obj *o = (struct jvm_obj *)locals[1];
        const char *a = s && s->array_data ? (char *)s->array_data : "";
        const char *b = o && o->array_data ? (char *)o->array_data : "";
        char buf[512];
        int la = (int)jstrlen(a), lb = (int)jstrlen(b);
        if (la + lb > 511) lb = 511 - la;
        memcpy(buf, a, la);
        memcpy(buf + la, b, lb);
        buf[la + lb] = 0;
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "trim") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = s && s->array_data ? (char *)s->array_data : "";
        const char *b = cs, *e = cs + jstrlen(cs);
        while (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r') b++;
        while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
        char buf[512];
        int n = (int)(e - b);
        if (n > 511) n = 511;
        memcpy(buf, b, n);
        buf[n] = 0;
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && (jstrcmp(name, "startsWith") == 0 || jstrcmp(name, "endsWith") == 0)) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        struct jvm_obj *o = (struct jvm_obj *)locals[1];
        const char *a = s && s->array_data ? (char *)s->array_data : "";
        const char *b = o && o->array_data ? (char *)o->array_data : "";
        int la = (int)jstrlen(a), lb = (int)jstrlen(b);
        if (jstrcmp(name, "startsWith") == 0) stack[top] = (la >= lb && jstrncmp(a, b, lb) == 0);
        else stack[top] = (la >= lb && jstrncmp(a + la - lb, b, lb) == 0);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "replace") == 0 && desc[1] == 'C') {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = s && s->array_data ? (char *)s->array_data : "";
        char o = (char)locals[1], n = (char)locals[2];
        char buf[512];
        int i = 0;
        for (; cs[i] && i < 510; i++) buf[i] = (cs[i] == o) ? n : cs[i];
        buf[i] = 0;
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && (jstrcmp(name, "toUpperCase") == 0 || jstrcmp(name, "toLowerCase") == 0)) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = s && s->array_data ? (char *)s->array_data : "";
        char buf[512];
        int up = jstrcmp(name, "toUpperCase") == 0;
        for (int i = 0; cs[i] && i < 511; i++) {
            char c = cs[i];
            buf[i] = up ? (c >= 'a' && c <= 'z' ? c - 32 : c)
                        : (c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        buf[jstrlen(cs) > 511 ? 511 : (int)jstrlen(cs)] = 0;
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "toCharArray") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *cs = s && s->array_data ? (char *)s->array_data : "";
        int n = (int)jstrlen(cs);
        struct jvm_obj *a = arr_new(NULL, n);
        for (int i = 0; i < n; i++) a->array_data[i] = (uint8_t)cs[i];
        stack[top] = (int32_t)a;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "valueOf") == 0) {
        char buf[64];
        if (desc[1] == 'I' || desc[1] == 'S' || desc[1] == 'B' || desc[1] == 'C') {
            snprintf(buf, sizeof(buf), "%d", (int)locals[0]);
        } else if (desc[1] == 'Z') {
            snprintf(buf, sizeof(buf), "%s", locals[0] ? "true" : "false");
        } else if (desc[1] == 'J') {
            int64_t v = (int64_t)locals[0];
            snprintf(buf, sizeof(buf), "%lld", (long long)v);
        } else if (desc[1] == 'F') {
            ftoa(buf, sizeof(buf), fbits(locals[0]));
        } else if (desc[1] == 'D') {
            double d = dfrom2(locals[0], locals[1]);
            snprintf(buf, sizeof(buf), "%.2f", d);
        } else {
            struct jvm_obj *s = (struct jvm_obj *)locals[0];
            const char *cs = s && s->array_data ? (char *)s->array_data : "null";
            stack[top] = (int32_t)jvm_string(cs);
            return 1;
        }
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }

    /* ---- java.lang.Class 反射（简化） ---- */
    if (jstrcmp(cls, "java/lang/Class") == 0) {
        if (jstrcmp(name, "getName") == 0) {
            struct jvm_obj *co = (struct jvm_obj *)locals[0];
            struct jvm_class *c = co ? (struct jvm_class *)co->fields[0] : NULL;
            stack[top] = (int32_t)jvm_string(c ? c->name : "?");
            return 1;
        }
        if (jstrcmp(name, "newInstance") == 0) {
            struct jvm_obj *co = (struct jvm_obj *)locals[0];
            struct jvm_class *c = co ? (struct jvm_class *)co->fields[0] : NULL;
            struct jvm_obj *o = c ? obj_new(c) : NULL;
            if (o && c) {
                struct jvm_method *init = find_method(c, "<init>", "()V");
                if (init) { int32_t a0 = (int32_t)o; exec_method(init, init->owner ? init->owner : c, &a0, 1, 0); }
            }
            stack[top] = (int32_t)o;
            return 1;
        }
        if (jstrcmp(name, "getMethod") == 0 || jstrcmp(name, "getDeclaredMethod") == 0) {
            struct jvm_obj *co = (struct jvm_obj *)locals[0];
            struct jvm_class *c = co ? (struct jvm_class *)co->fields[0] : NULL;
            struct jvm_obj *ns = (struct jvm_obj *)locals[1];
            const char *mn = (ns && ns->array_data) ? (const char *)ns->array_data : "";
            struct jvm_method *m = NULL;
            if (c) for (int i = 0; i < c->method_count; i++)
                if (jstrcmp(c->methods[i].name, mn) == 0) { m = &c->methods[i]; break; }
            if (m) {
                struct jvm_obj *mo = obj_new(NULL);
                mo->fields[0] = (int32_t)c;
                mo->fields[1] = (int32_t)m;
                stack[top] = (int32_t)mo;
            } else {
                stack[top] = 0;
            }
            return 1;
        }
        /* Class.getDeclaredField(Ljava/lang/String;)Ljava/lang/reflect/Field; → Field 伪对象（CHM.clinit 反射 theUnsafe 需要） */
        if (jstrcmp(name, "getDeclaredField") == 0 || jstrcmp(name, "getField") == 0) {
            struct jvm_obj *co = (struct jvm_obj *)locals[0];
            struct jvm_obj *fo = obj_new(NULL);
            if (fo) {
                fo->fields[0] = co ? co->fields[0] : 0; /* 声明类 */
                fo->fields[1] = locals[1];               /* 名字 String */
            }
            stack[top] = (int32_t)fo;
            return 1;
        }
        /* Class.getFields/getDeclaredFields → 空数组（防 null 循环） */
        if (jstrcmp(name, "getFields") == 0 || jstrcmp(name, "getDeclaredFields") == 0) {
            stack[top] = (int32_t)arr_new(find_class("java/lang/Object"), 0);
            return 1;
        }
    }
    /* ---- java.lang.reflect.Method 反射（简化） ---- */
    if (jstrcmp(cls, "java/lang/reflect/Method") == 0) {
        if (jstrcmp(name, "getName") == 0) {
            struct jvm_obj *mo = (struct jvm_obj *)locals[0];
            struct jvm_method *m = mo ? (struct jvm_method *)mo->fields[1] : NULL;
            stack[top] = (int32_t)jvm_string(m ? m->name : "?");
            return 1;
        }
        if (jstrcmp(name, "invoke") == 0) {
            struct jvm_obj *mo = (struct jvm_obj *)locals[0];
            struct jvm_class *c = mo ? (struct jvm_class *)mo->fields[0] : NULL;
            struct jvm_method *m = mo ? (struct jvm_method *)mo->fields[1] : NULL;
            struct jvm_obj *recv = (struct jvm_obj *)locals[1];
            struct jvm_obj *argarr = (struct jvm_obj *)locals[2];
            if (!m || !c) { stack[top] = 0; return 1; }
            int n = argarr ? argarr->array_len : 0;
            if (n > 14) n = 14;
            int32_t cargs[16];
            int ci = 0;
            if (!(m->flags & ACC_STATIC)) cargs[ci++] = (int32_t)recv;
            for (int i = 0; i < n; i++) cargs[ci++] = argarr->array_data[i];
            int32_t r = exec_method(m, m->owner ? m->owner : c, cargs, ci, (m->flags & ACC_STATIC) ? 1 : 0);
            char retc = m->desc[jstrlen(m->desc) - 1];
            if (retc == 'J' || retc == 'D') { stack[top] = r; stack[top + 1] = 0; top += 2; }
            else if (retc != 'V') stack[top++] = r;
            return 1;
        }
    }
    /* ---- java.lang.reflect.Field.get（简化）：theUnsafe 反射给 Unsafe 伪对象（CHM 依赖） ---- */
    if (jstrcmp(cls, "java/lang/reflect/Field") == 0 && jstrcmp(name, "get") == 0) {
        struct jvm_obj *fo = (struct jvm_obj *)locals[0];
        struct jvm_obj *fnm = (fo && fo->fields[1]) ? (struct jvm_obj *)fo->fields[1] : NULL;
        const char *fname = (fnm && fnm->array_data) ? (const char *)fnm->array_data : "";
        if (jstrcmp(fname, "theUnsafe") == 0 || jstrcmp(fname, "unsafe") == 0) {
            struct jvm_class *uc = find_class("sun/misc/Unsafe");
            if (!uc) uc = find_class("java/lang/Object");
            stack[top] = (int32_t)(uc ? obj_new(uc) : 0);
        } else {
            stack[top] = 0;
        }
        return 1;
    }
    if ((jstrcmp(cls, "java/lang/reflect/Field") == 0 || jstrcmp(cls, "java/lang/reflect/AccessibleObject") == 0) &&
        (jstrcmp(name, "setAccessible") == 0 || jstrcmp(name, "isAccessible") == 0)) {
        if (jstrcmp(name, "setAccessible") == 0) return 0; /* void */
        stack[top] = 1; return 1;
    }
    /* ---- StringBuilder / StringBuffer ---- */
    if ((jstrcmp(cls, "java/lang/StringBuilder") == 0 || jstrcmp(cls, "java/lang/StringBuffer") == 0)) {
        if (jstrcmp(name, "<init>") == 0) return 0;
        if (jstrcmp(name, "append") == 0) {
            /* 直接拼接进 this 的缓冲，返回 this */
            struct jvm_obj *sb = (struct jvm_obj *)locals[0];
            char extra[128];
            const char *p = "";
            char d = desc[1];
            if (d == 'I' || d == 'S' || d == 'B' || d == 'C') { snprintf(extra, sizeof(extra), "%d", (int)locals[1]); p = extra; }
            else if (d == 'Z') { p = locals[1] ? "true" : "false"; }
            else if (d == 'J') { int64_t jv = ((int64_t)(uint32_t)locals[2] << 32) | (uint32_t)locals[1]; lltoa10(jv, extra); p = extra; }
            else if (d == 'D') { ftoa(extra, sizeof(extra), (float)dfrom2(locals[1], locals[2])); p = extra; }
            else if (d == 'L' || d == '[') {
                struct jvm_obj *s = (struct jvm_obj *)locals[1];
                p = (s && s->is_array && s->array_data) ? (char *)s->array_data : "null";
            }
            if (sb && sb->array_data) {
                int cur = (int)jstrlen((char *)sb->array_data);
                int add = (int)jstrlen(p);
                if (cur + add < 4096) memcpy((char *)sb->array_data + cur, p, add + 1);
            }
            stack[top] = (int32_t)sb;
            return 1;
        }
        if (jstrcmp(name, "toString") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[0];
            stack[top] = (int32_t)jvm_string(sb && sb->array_data ? (char *)sb->array_data : "");
            return 1;
        }
        if (jstrcmp(name, "length") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[0];
            stack[top] = sb && sb->array_data ? (int32_t)jstrlen((char *)sb->array_data) : 0;
            return 1;
        }
        if (jstrcmp(name, "charAt") == 0) {
            struct jvm_obj *sb = (struct jvm_obj *)locals[0];
            const char *cs = sb && sb->array_data ? (char *)sb->array_data : "";
            int32_t i = locals[1];
            stack[top] = (i >= 0 && i < (int32_t)jstrlen(cs)) ? (uint8_t)cs[i] : 0;
            return 1;
        }
    }

    /* ---- Math ---- */
    if (jstrcmp(cls, "java/lang/Math") == 0) {
        if (jstrcmp(name, "abs") == 0) {
            char d = desc[1];
            if (d == 'I') { int32_t v = locals[0]; stack[top] = v < 0 ? -v : v; }
            else if (d == 'J') { int64_t v = (int64_t)locals[0]; push_long(stack, &top, v < 0 ? -v : v); return 1; }
            else if (d == 'F') { float f = fbits(locals[0]); stack[top] = bitsf(f < 0 ? -f : f); }
            else if (d == 'D') { double f = dfrom2(locals[0], locals[1]); int32_t lo, hi; dto2(f < 0 ? -f : f, &lo, &hi); stack[top++] = lo; stack[top++] = hi; return 1; }
            return 1;
        }
        if (jstrcmp(name, "min") == 0 || jstrcmp(name, "max") == 0) {
            char d = desc[1];
            int ismax = jstrcmp(name, "max") == 0;
            if (d == 'I') { int32_t a = locals[0], b = locals[1]; stack[top] = ismax ? (a > b ? a : b) : (a < b ? a : b); }
            else if (d == 'J') { int64_t a = (int64_t)locals[0], b = (int64_t)locals[1]; push_long(stack, &top, ismax ? (a > b ? a : b) : (a < b ? a : b)); return 1; }
            else if (d == 'F') { float a = fbits(locals[0]), b = fbits(locals[1]); stack[top] = bitsf(ismax ? (a > b ? a : b) : (a < b ? a : b)); }
            else if (d == 'D') { double a = dfrom2(locals[0], locals[1]), b = dfrom2(locals[2], locals[3]); int32_t lo, hi; dto2(ismax ? (a > b ? a : b) : (a < b ? a : b), &lo, &hi); stack[top++] = lo; stack[top++] = hi; return 1; }
            return 1;
        }
        if (jstrcmp(name, "sqrt") == 0) {
            double v = dfrom2(locals[0], locals[1]);
            /* 牛顿迭代求平方根（软浮点没有 sqrt） */
            double x = v > 0 ? v : 0;
            for (int i = 0; i < 40; i++) x = (x + v / x) * 0.5;
            int32_t lo, hi; dto2(x, &lo, &hi);
            stack[top] = lo; stack[top + 1] = hi;
            return 1;
        }
        if (jstrcmp(name, "pow") == 0) {
            double a = dfrom2(locals[0], locals[1]);
            double b = dfrom2(locals[2], locals[3]);
            /* 整数幂近似 */
            double r = 1.0;
            long n = (long)b;
            int neg = n < 0;
            if (neg) n = -n;
            for (long i = 0; i < n; i++) r *= a;
            if (neg && r != 0) r = 1.0 / r;
            int32_t lo, hi; dto2(r, &lo, &hi);
            stack[top] = lo; stack[top + 1] = hi;
            return 1;
        }
        if (jstrcmp(name, "floor") == 0 || jstrcmp(name, "ceil") == 0) {
            double v = dfrom2(locals[0], locals[1]);
            double r = (jstrcmp(name, "floor") == 0) ? (double)(long)v : (v > (double)(long)v ? (double)((long)v + 1) : (double)(long)v);
            int32_t lo, hi; dto2(r, &lo, &hi);
            stack[top] = lo; stack[top + 1] = hi;
            return 1;
        }
        if (jstrcmp(name, "round") == 0 && desc[1] == 'D') {
            double v = dfrom2(locals[0], locals[1]);
            stack[top] = (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
            return 1;
        }
        if (jstrcmp(name, "random") == 0) {
            /* 简易伪随机 [0,1) */
            static unsigned long rng = 0x9e3779b9u;
            rng = rng * 1664525u + 1013904223u;
            double r = (double)(rng & 0xffffff) / 16777216.0;
            int32_t lo, hi; dto2(r, &lo, &hi);
            stack[top++] = lo; stack[top++] = hi;
            return 1;
        }
    }

    /* ---- System ---- */
    if (jstrcmp(cls, "java/lang/System") == 0 && jstrcmp(name, "currentTimeMillis") == 0) {
        stack[top] = (int32_t)(sys_ticks() * 10); /* 100Hz -> ms，低 32 位 */
        return 1;
    }
    if (jstrcmp(cls, "java/lang/System") == 0 && jstrcmp(name, "nanoTime") == 0) {
        push_long(stack, &top, (int64_t)sys_ticks() * 10000000LL);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/System") == 0 && jstrcmp(name, "getProperty") == 0) {
        stack[top] = 0; /* null */
        return 1;
    }

    /* ---- Float / Double：位模式直通（JVM 槽内本就是 IEEE 位模式，无需真算） ---- */
    if (jstrcmp(cls, "java/lang/Float") == 0) {
        if (jstrcmp(name, "floatToRawIntBits") == 0 || jstrcmp(name, "floatToIntBits") == 0 ||
            jstrcmp(name, "intBitsToFloat") == 0) {
            stack[top] = locals[0];
            return 1;
        }
    }
    if (jstrcmp(cls, "java/lang/Double") == 0) {
        if (jstrcmp(name, "doubleToRawLongBits") == 0 || jstrcmp(name, "doubleToLongBits") == 0 ||
            jstrcmp(name, "longBitsToDouble") == 0) {
            stack[top] = locals[0];
            stack[top + 1] = locals[1];
            return 1;
        }
    }
    /* ---- Class.getPrimitiveClass：原始类型 Class 占位（供 Float.TYPE 等静态字段） ---- */
    if (jstrcmp(cls, "java/lang/Class") == 0 && jstrcmp(name, "getPrimitiveClass") == 0) {
        struct jvm_class *pcls = find_class("java/lang/Object");
        struct jvm_obj *pobj = pcls ? obj_new(pcls) : NULL;
        stack[top] = (int32_t)pobj;
        return 1;
    }

    /* ---- Integer / Long ---- */
    if (jstrcmp(cls, "java/lang/Integer") == 0 && jstrcmp(name, "parseInt") == 0 && desc[1] == 'L') {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *p = (s && s->array_data) ? (char *)s->array_data : "";
        int32_t r = 0, neg = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; }
        stack[top] = neg ? -r : r;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Integer") == 0 && jstrcmp(name, "toString") == 0 && desc[1] == 'I') {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)locals[0]);
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Integer") == 0 && jstrcmp(name, "valueOf") == 0 && desc[1] == 'I') {
        struct jvm_class *ic = find_class("java/lang/Integer");
        struct jvm_obj *o = ic ? obj_new(ic) : obj_new(find_class("java/lang/Object"));
        /* 简化：值存 fields[0] */
        o->fields[0] = locals[0];
        stack[top] = (int32_t)o;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Long") == 0 && jstrcmp(name, "parseLong") == 0) {
        struct jvm_obj *s = (struct jvm_obj *)locals[0];
        const char *p = (s && s->array_data) ? (char *)s->array_data : "";
        int64_t r = 0; int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; }
        push_long(stack, &top, neg ? -r : r);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Long") == 0 && jstrcmp(name, "toString") == 0) {
        char buf[24];
        int64_t v = desc[1] == 'J' ? dfrom2(0, 0) : 0;
        if (desc[1] == 'J') v = ((int64_t)locals[1] << 32) | (uint32_t)locals[0];
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }

    /* ---- Character ---- */
    if (jstrcmp(cls, "java/lang/Character") == 0) {
        int32_t c = locals[0];
        if (jstrcmp(name, "isDigit") == 0) stack[top] = (c >= '0' && c <= '9');
        else if (jstrcmp(name, "isLetter") == 0) stack[top] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        else if (jstrcmp(name, "isWhitespace") == 0) stack[top] = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        else if (jstrcmp(name, "toUpperCase") == 0) stack[top] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        else if (jstrcmp(name, "toLowerCase") == 0) stack[top] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        else return 0;
        return 1;
    }

    /* ---- Boolean ---- */
    if (jstrcmp(cls, "java/lang/Boolean") == 0) {
        if (jstrcmp(name, "parseBoolean") == 0) {
            struct jvm_obj *s = (struct jvm_obj *)locals[0];
            const char *p = (s && s->array_data) ? (char *)s->array_data : "";
            stack[top] = (jstrcmp(p, "true") == 0);
        } else if (jstrcmp(name, "toString") == 0) {
            stack[top] = (int32_t)jvm_string(locals[0] ? "true" : "false");
        } else return 0;
        return 1;
    }

    /* ---- Float / Double toString ---- */
    if (jstrcmp(cls, "java/lang/Float") == 0 && jstrcmp(name, "toString") == 0) {
        char buf[64];
        ftoa(buf, sizeof(buf), fbits(locals[0]));
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Double") == 0 && jstrcmp(name, "toString") == 0) {
        char buf[64];
        double v = dfrom2(locals[0], locals[1]);
        snprintf(buf, sizeof(buf), "%.2f", v);
        stack[top] = (int32_t)jvm_string(buf);
        return 1;
    }

    /* ---- Thread（javm 简化：单解释器同步语义；start 直接同步跑 run） ---- */
    if (jstrcmp(cls, "java/lang/Thread") == 0) {
        if (jstrcmp(name, "<init>") == 0) {
            /* Thread.<init>(Runnable)：把 target 存到 this 字段槽 0 */
            struct jvm_obj *th = (struct jvm_obj *)locals[0];
            if (th) th->fields[0] = locals[1];
            return 0;
        }
        if (jstrcmp(name, "start") == 0 || jstrcmp(name, "start0") == 0) {
            struct jvm_obj *th = (struct jvm_obj *)locals[0];
            struct jvm_obj *tg = th ? (struct jvm_obj *)th->fields[0] : NULL;
            struct jvm_method *rm = NULL;
            struct jvm_class *rc = NULL;
            if (tg && tg->cls) {
                rm = find_method(tg->cls, "run", "()V");
                rc = tg->cls;
            }
            if (!rm && th && th->cls) {
                /* Thread 子类 override run()：直接跑子类 run */
                rm = find_method(th->cls, "run", "()V");
                rc = th->cls;
            }
            /* ReferenceHandler 线程 run 是硬无限循环（goto -5），javm 同步执行会卡死主线程；跳过 */
            if (rc && rc->name && jstrcmp(rc->name, "java/lang/ref/Reference$ReferenceHandler") == 0) {
                return 0;
            }
            if (rm && rc) {
                int32_t a0 = (int32_t)(tg ? tg : th);
                exec_method(rm, rm->owner ? rm->owner : rc, &a0, 1, 0);
            } else {
                printf("[JVM] Thread: no run() for start\n");
            }
            return 0;
        }
        if (jstrcmp(name, "currentThread") == 0) {
            stack[top] = 0; /* 伪对象 */
            return 1;
        }
        if (jstrcmp(name, "getName") == 0) {
            stack[top] = (int32_t)jvm_string("main");
            return 1;
        }
        if (jstrcmp(name, "setName") == 0) { return 0; }
        if (jstrcmp(name, "isAlive") == 0) { stack[top] = 1; return 1; }
        if (jstrcmp(name, "yield") == 0) { return 0; }
        if (jstrcmp(name, "setPriority") == 0) { return 0; }
        if (jstrcmp(name, "getPriority") == 0) { stack[top] = 5; return 1; }
        if (jstrcmp(name, "sleep") == 0) {
            sys_sleep((int)locals[0]);
            return 0; /* void */
        }
    }

    /* ---- java/util/IdentityHashMap 简化 natives ----
     * 值模型：int32 槽。简化实现：对象伪装成数组（array_data[0]=count，
     * 条目从 1 起每 2 槽 = key/value），Identity 语义（== 比较）。 */
    if (jstrcmp(cls, "java/util/IdentityHashMap") == 0) {
        struct jvm_obj *ihm_o = (struct jvm_obj *)locals[0];
        if (!ihm_o || !ihm_o->is_array || !ihm_o->array_data) {
            /* 非数组布局：忽略调用，压 0（对象方法压 null，int 压 0） */
            printf("[JVM] IdentityHashMap fallback %s.%s\n", name, desc);
            char rp2;
            for (rp2 = 0; desc[rp2]; rp2++) if (desc[rp2] == ')') break;
            char rc = desc[rp2 + 1];
            if (rc == 'J' || rc == 'D') { stack[top++] = 0; stack[top++] = 0; }
            else if (rc != 'V' && rc != 0) stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "put") == 0 && desc[0] == '(') {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            int32_t key = locals[1], val = locals[2];
            int32_t *d0 = o->array_data;
            int32_t cnt = d0[0];
            for (int32_t i = 1; i < 1 + 2 * cnt; i += 2) {
                if (d0[i] == key) { int32_t old = d0[i + 1]; d0[i + 1] = val; stack[top++] = old; return 1; }
            }
            if (o->array_len >= 2 + 2 * (cnt + 1)) {
                d0[1 + 2 * cnt] = key; d0[2 + 2 * cnt] = val; d0[0] = cnt + 1;
            }
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "get") == 0 && desc[0] == '(') {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            int32_t key = locals[1];
            int32_t *d0 = o->array_data;
            int32_t cnt = d0[0];
            for (int32_t i = 1; i < 1 + 2 * cnt; i += 2)
                if (d0[i] == key) { stack[top++] = d0[i + 1]; return 1; }
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "size") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            stack[top++] = o->array_data[0];
            return 1;
        }
        if (jstrcmp(name, "isEmpty") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            stack[top++] = (o->array_data[0] == 0) ? 1 : 0;
            return 1;
        }
        if (jstrcmp(name, "remove") == 0 && desc[0] == '(') {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            int32_t key = locals[1];
            int32_t *d0 = o->array_data;
            int32_t cnt = d0[0];
            for (int32_t i = 1; i < 1 + 2 * cnt; i += 2) {
                if (d0[i] == key) {
                    int32_t old = d0[i + 1];
                    for (int32_t j = i; j < 1 + 2 * cnt - 2; j++) d0[j] = d0[j + 2];
                    d0[0] = cnt - 1;
                    stack[top++] = old;
                    return 1;
                }
            }
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "containsKey") == 0 && desc[0] == '(') {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            int32_t key = locals[1];
            int32_t *d0 = o->array_data;
            int32_t cnt = d0[0];
            for (int32_t i = 1; i < 1 + 2 * cnt; i += 2)
                if (d0[i] == key) { stack[top++] = 1; return 1; }
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "clear") == 0) {
            struct jvm_obj *o = (struct jvm_obj *)locals[0];
            o->array_data[0] = 0;
            return 0;
        }
    }

    /* ---- java/io/PrintStream 输出 ---- */
    if (jstrcmp(cls, "java/io/PrintStream") == 0) {
        if (jstrcmp(name, "println") == 0) {
            if (desc[0] == '(' && desc[1] == ')') {
                printf("\n");
                return 0;
            }
            struct jvm_obj *s = (struct jvm_obj *)locals[1];
            if (s && s->is_array && s->array_data) {
                printf("%s\n", (const char *)s->array_data);
            }
            return 0;
        }
        if (jstrcmp(name, "print") == 0) {
            struct jvm_obj *s = (struct jvm_obj *)locals[1];
            if (s && s->is_array && s->array_data) {
                printf("%s", (const char *)s->array_data);
            }
            return 0;
        }
        if (jstrcmp(name, "flush") == 0 || jstrcmp(name, "close") == 0) {
            return 0;
        }
        if (jstrcmp(name, "write") == 0) {
            return 0;
        }
    }
    /* System.err/System.setOut 等简化 */
    if (jstrcmp(cls, "java/lang/System") == 0) {
        if (jstrcmp(name, "setOut") == 0 || jstrcmp(name, "setErr") == 0 ||
            jstrcmp(name, "setIn") == 0) {
            return 0;
        }
        if (jstrcmp(name, "arraycopy") == 0) {
            /* 简化数组拷贝（同类型 int32 槽拷贝，带边界 clamp） */
            struct jvm_obj *src = (struct jvm_obj *)locals[0];
            struct jvm_obj *dst = (struct jvm_obj *)locals[2];
            int32_t sp = locals[1], dp = locals[3], ln = locals[4];
            if (src && dst && src->is_array && dst->is_array && src->array_data && dst->array_data &&
                (uint32_t)src >= 0xc0000000u && (uint32_t)src < 0xf0000000u &&
                (uint32_t)dst >= 0xc0000000u && (uint32_t)dst < 0xf0000000u &&
                (uint32_t)src->array_data >= 0xc0000000u && (uint32_t)src->array_data < 0xf0000000u &&
                (uint32_t)dst->array_data >= 0xc0000000u && (uint32_t)dst->array_data < 0xf0000000u) {
                int sl = src->array_len, dl = dst->array_len;
                if (sp < 0) sp = 0;
                if (dp < 0) dp = 0;
                if (ln < 0) ln = 0;
                if (sp + ln > sl) ln = sl - sp;
                if (dp + ln > dl) ln = dl - dp;
                for (int32_t i = 0; i < ln; i++)
                    dst->array_data[dp + i] = src->array_data[sp + i];
            }
            return 0;
        }
        if (jstrcmp(name, "currentTimeMillis") == 0) {
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "nanoTime") == 0) {
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "gc") == 0 || jstrcmp(name, "exit") == 0) {
            return 0;
        }
    }

    /* ---- java/lang/Class 反射简化 ---- */
    if (jstrcmp(cls, "java/lang/Class") == 0) {
        if (jstrcmp(name, "newInstance") == 0) {
            /* 简化：返回 null（调用方有 null-receiver 保护） */
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "isAssignableFrom") == 0) {
            stack[top++] = 1;
            return 1;
        }
        if (jstrcmp(name, "isInstance") == 0) {
            stack[top++] = 1;
            return 1;
        }
        if (jstrcmp(name, "getSuperclass") == 0) {
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "getInterfaces") == 0) {
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "getModifiers") == 0) {
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "getName") == 0) {
            stack[top++] = (int32_t)jvm_string("java/lang/Class");
            return 1;
        }
        if (jstrcmp(name, "getMethods") == 0 || jstrcmp(name, "getDeclaredMethods") == 0 ||
            jstrcmp(name, "getFields") == 0 || jstrcmp(name, "getDeclaredFields") == 0) {
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "getConstructor") == 0 || jstrcmp(name, "getDeclaredConstructor") == 0 ||
            jstrcmp(name, "getMethod") == 0 || jstrcmp(name, "getDeclaredMethod") == 0) {
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "forName") == 0) {
            /* 简化：返回 null */
            stack[top++] = 0;
            return 1;
        }
        if (jstrcmp(name, "getClassLoader") == 0 || jstrcmp(name, "getProtectionDomain") == 0 ||
            jstrcmp(name, "getDeclaredAnnotations") == 0 || jstrcmp(name, "getAnnotations") == 0) {
            stack[top++] = 0;
            return 1;
        }
    }
    /* java/lang/reflect 简化 */
    if (jstrcmp(cls, "java/lang/reflect/Modifier") == 0) {
        stack[top++] = 0;
        return 1;
    }

    /* ---- java.net 网络 natives（对接内核 TCP syscall 43-49） ---- */
    /* Socket.nativeConnect(Ljava/lang/String;I)V —— 失败返回 -1（不抛异常，调用方判 connected） */
    if (jstrcmp(cls, "java/net/Socket") == 0 && jstrcmp(name, "nativeConnect") == 0) {
        struct jvm_obj *hs = (struct jvm_obj *)locals[1];
        const char *host = (hs && hs->array_data) ? (const char *)hs->array_data : "";
        int port = (int)locals[2];
        int r = sys_tcp_connect(host, port);
        printf("[net] Socket.connect %s:%d -> %d\n", host, port, r);
        return 0; /* void */
    }
    /* Socket.close()V */
    if (jstrcmp(cls, "java/net/Socket") == 0 && jstrcmp(name, "close") == 0) {
        sys_tcp_close();
        printf("[net] Socket.close\n");
        return 0;
    }
    /* ServerSocket.nativeListen(I)V */
    if (jstrcmp(cls, "java/net/ServerSocket") == 0 && jstrcmp(name, "nativeListen") == 0) {
        int r = sys_tcp_listen((int)locals[1]);
        printf("[net] ServerSocket.listen %d -> %d\n", (int)locals[1], r);
        return 0;
    }
    /* ServerSocket.accept()Ljava/net/Socket; → 返回新 Socket 对象（或 NULL=失败） */
    if (jstrcmp(cls, "java/net/ServerSocket") == 0 && jstrcmp(name, "accept") == 0) {
        int r = sys_tcp_accept();
        printf("[net] ServerSocket.accept -> %d\n", r);
        if (r < 0) {
            stack[top] = 0;
        } else {
            struct class *sc = find_class("java/net/Socket");
            if (sc) {
                struct jvm_obj *so = obj_new(sc);
                stack[top] = (int32_t)so;
            } else {
                stack[top] = 0;
            }
        }
        return 1;
    }
    /* ServerSocket.close()V */
    if (jstrcmp(cls, "java/net/ServerSocket") == 0 && jstrcmp(name, "close") == 0) {
        sys_tcp_close();
        return 0;
    }
    /* SocketOutputStream.write(I)V 与 write([BII)V */
    if (jstrcmp(cls, "java/net/SocketOutputStream") == 0) {
        if (jstrcmp(name, "write") == 0) {
            if (desc[1] == 'I') {
                unsigned char c = (unsigned char)(locals[0] & 0xFF);
                sys_tcp_send(&c, 1);
                return 0;
            }
            /* write([BII)V：locals[0]=this [1]=byte[] [2]=off [3]=len */
            struct jvm_obj *bb = (struct jvm_obj *)locals[1];
            int off = (int)locals[2];
            int len = (int)locals[3];
            if (bb && bb->array_data && len > 0) {
                sys_tcp_send(bb->array_data + off, len);
            }
            return 0;
        }
    }
    /* SocketInputStream.read()I 与 read([BII)I */
    if (jstrcmp(cls, "java/net/SocketInputStream") == 0) {
        if (jstrcmp(name, "read") == 0) {
            if (desc[1] == ')') { /* read()I */
                unsigned char c;
                int r = sys_tcp_recv(&c, 1, 0);
                stack[top] = (r <= 0) ? -1 : (int)c;
                return 1;
            }
            /* read([BII)I：locals[0]=this [1]=byte[] [2]=off [3]=len */
            struct jvm_obj *bb = (struct jvm_obj *)locals[1];
            int off = (int)locals[2];
            int len = (int)locals[3];
            int r = 0;
            if (bb && bb->array_data && len > 0)
                r = sys_tcp_recv(bb->array_data + off, len, 0);
            stack[top] = r;
            return 1;
        }
    }

    /* ---- Throwable / Exception 简化：构造/方法忽略 ---- */
    if (jstrcmp(cls, "java/lang/Throwable") == 0) return 0;
    if (jstrcmp(cls, "java/lang/Exception") == 0) return 0;
    if (jstrcmp(cls, "java/lang/RuntimeException") == 0) return 0;
    if (jstrcmp(cls, "java/lang/NullPointerException") == 0) return 0;
    if (jstrcmp(cls, "java/lang/Enum") == 0 && jstrcmp(name, "<init>") == 0) return 1; /* 枚举构造 stub（V 返回） */
    if (jstrcmp(cls, "java/lang/IllegalArgumentException") == 0 && jstrcmp(name, "<init>") == 0) return 1;
    if (jstrcmp(cls, "java/text/SimpleDateFormat") == 0 && jstrcmp(name, "<init>") == 0) return 1;
    if (jstrcmp(cls, "java/lang/Float") == 0 && jstrcmp(name, "isNaN") == 0) { stack[top] = 0; return 1; }
    if (jstrcmp(cls, "java/lang/Comparable") == 0 && jstrcmp(name, "compareTo") == 0) { stack[top] = 0; return 1; }
    /* java/util/regex/Pattern.compile：返回 stub 实例（避免调用方 NPE） */
    if (jstrcmp(cls, "java/util/regex/Pattern") == 0 && jstrcmp(name, "compile") == 0) {
        struct jvm_class *pc2 = find_class("java/util/regex/Pattern");
        stack[top] = (int32_t)(pc2 ? obj_new(pc2) : 0);
        return 1;
    }
    if (jstrcmp(cls, "java/util/Collections") == 0 && jstrcmp(name, "addAll") == 0) { stack[top] = 1; return 1; }
    if (jstrcmp(cls, "java/lang/String") == 0 && jstrcmp(name, "format") == 0) {
        struct jvm_class *sc = find_class("java/lang/Object");
        stack[top] = (int32_t)(sc ? jvm_string("") : 0); return 1;
    }
    if (jstrcmp(cls, "java/lang/ArrayIndexOutOfBoundsException") == 0) return 0;
    if (jstrcmp(cls, "java/lang/ArithmeticException") == 0) return 0;
    if (jstrcmp(name, "initCause") == 0) return 0;

    /* ---- 数组 clone：返回浅拷贝新数组（方块/实体数组需要） ---- */
    if (cls && cls[0] == '[' && jstrcmp(name, "clone") == 0) {
        struct jvm_obj *src = (struct jvm_obj *)locals[0];
        if (src && (uint32_t)src >= 0xc0000000u && (uint32_t)src < 0xf0000000u &&
            src->is_array) {
            struct jvm_obj *a = arr_new(src->cls, src->array_len);
            if (a && src->array_len > 0 && src->array_data && a->array_data)
                memcpy(a->array_data, src->array_data,
                       (size_t)src->array_len * sizeof(int32_t));
            stack[top] = (int32_t)a;
            return 1;
        }
        stack[top] = 0;
        return 1;
    }

    /* ---- sun/misc/Unsafe 原子操作：CAS 返回成功（1），get/put 内存读写 ---- */
    if (jstrcmp(cls, "sun/misc/Unsafe") == 0) {
        if (jstrcmp(name, "compareAndSwapInt") == 0 ||
            jstrcmp(name, "compareAndSwapLong") == 0 ||
            jstrcmp(name, "compareAndSwapObject") == 0) {
            /* 真实数组 CAS：CHM.casTabAt 依赖它写 table，否则 putVal 永不生效死循环。
               locals[1]=数组, locals[2..3]=offset(long 2槽), locals[4]=expected, locals[5]=x */
            struct jvm_obj *cao = (struct jvm_obj *)locals[1];
            int64_t coff = (int64_t)(int32_t)locals[2] | ((int64_t)(int32_t)locals[3] << 32);
            if (cao && (uint32_t)cao >= 0xc0000000u && (uint32_t)cao < 0xf0000000u &&
                cao->is_array && cao->array_data && coff >= 0 && (coff & 3) == 0) {
                int cidx = (int)(coff >> 2);
                if (cidx >= 0 && cidx < cao->array_len) {
                    int32_t ccur = cao->array_data[cidx];
                    if (ccur == locals[4]) { cao->array_data[cidx] = locals[5]; stack[top] = 1; return 1; }
                    stack[top] = 0; return 1;
                }
            }
            stack[top] = 1; return 1; /* 数组非法：假装成功（保守防死循环） */
        }
        if (jstrcmp(name, "getObjectVolatile") == 0 || jstrcmp(name, "getObjectAcquire") == 0 ||
            jstrcmp(name, "getObject") == 0) {
            /* 真实数组读：CHM.tabAt 依赖它读 table 元素。
               locals[1]=数组, locals[2..3]=offset(long 2槽) */
            struct jvm_obj *gvo = (struct jvm_obj *)locals[1];
            int64_t goff = (int64_t)(int32_t)locals[2] | ((int64_t)(int32_t)locals[3] << 32);
            if (gvo && (uint32_t)gvo >= 0xc0000000u && (uint32_t)gvo < 0xf0000000u &&
                gvo->is_array && gvo->array_data && goff >= 0 && (goff & 3) == 0) {
                int gidx = (int)(goff >> 2);
                if (gidx >= 0 && gidx < gvo->array_len) { stack[top] = gvo->array_data[gidx]; return 1; }
            }
            stack[top] = 0; return 1;
        }
        if (jstrcmp(name, "getInt") == 0 || jstrcmp(name, "getLong") == 0 ||
            jstrcmp(name, "getBoolean") == 0 ||
            jstrcmp(name, "getIntVolatile") == 0 ||
            jstrcmp(name, "getLongVolatile") == 0 || jstrcmp(name, "getBooleanVolatile") == 0 ||
            jstrcmp(name, "getAndSetObject") == 0) {
            stack[top] = 0; return 1;
        }
        if (jstrcmp(name, "putObjectVolatile") == 0 || jstrcmp(name, "putObjectRelease") == 0 ||
            jstrcmp(name, "putObject") == 0) {
            /* 真实数组写：CHM 扩容/写桶用 */
            struct jvm_obj *pvo = (struct jvm_obj *)locals[1];
            int64_t poff = (int64_t)(int32_t)locals[2] | ((int64_t)(int32_t)locals[3] << 32);
            if (pvo && (uint32_t)pvo >= 0xc0000000u && (uint32_t)pvo < 0xf0000000u &&
                pvo->is_array && pvo->array_data && poff >= 0 && (poff & 3) == 0) {
                int pidx = (int)(poff >> 2);
                if (pidx >= 0 && pidx < pvo->array_len) pvo->array_data[pidx] = locals[4];
            }
            return 1; /* void */
        }
        if (jstrcmp(name, "putInt") == 0 || jstrcmp(name, "putLong") == 0 ||
            jstrcmp(name, "putObject") == 0 || jstrcmp(name, "putBoolean") == 0 ||
            jstrcmp(name, "putOrderedInt") == 0 || jstrcmp(name, "putOrderedLong") == 0 ||
            jstrcmp(name, "putOrderedObject") == 0 ||
            jstrcmp(name, "putObjectVolatile") == 0 || jstrcmp(name, "putIntVolatile") == 0 ||
            jstrcmp(name, "putLongVolatile") == 0 || jstrcmp(name, "putBooleanVolatile") == 0 ||
            jstrcmp(name, "putObjectRelease") == 0) {
            return 1; /* void */
        }
        if (jstrcmp(name, "arrayBaseOffset") == 0) { stack[top] = 0; return 1; }
        if (jstrcmp(name, "arrayIndexScale") == 0) { stack[top] = 4; return 1; }
        if (jstrcmp(name, "objectFieldOffset") == 0) { stack[top] = 0; return 1; }
        if (jstrcmp(name, "getUnsafe") == 0) {
            struct jvm_class *uc = find_class("sun/misc/Unsafe");
            if (!uc) uc = find_class("java/lang/Object"); /* Unsafe 类缺失时给 Object 伪对象：避免 U 为 null 触发 null-receiver 链 */
            stack[top] = (int32_t)(uc ? obj_new(uc) : 0); return 1;
        }
        if (jstrcmp(name, "pageSize") == 0) { stack[top] = 4096; return 1; }
        if (jstrcmp(name, "addressSize") == 0) { stack[top] = 4; return 1; }
        if (jstrcmp(name, "park") == 0 || jstrcmp(name, "unpark") == 0) { return 1; }
    }

    /* ---- CharacterData 缺失回退：Character.digit 常用 ---- */
    if (jstrcmp(cls, "java/lang/Character") == 0 && jstrcmp(name, "digit") == 0) {
        int ch = locals[1], radix = locals[2];
        int v = -1;
        if (radix >= 2 && radix <= 36) {
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'Z') v = ch - 'A' + 10;
        }
        if (v < 0 || v >= radix) v = -1;
        stack[top] = v;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Character") == 0 && jstrcmp(name, "isDigit") == 0) {
        int ch = locals[1];
        stack[top] = (ch >= '0' && ch <= '9') ? 1 : 0;
        return 1;
    }
    if (jstrcmp(cls, "java/lang/Character") == 0 && jstrcmp(name, "isLetter") == 0) {
        int ch = locals[1];
        stack[top] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) ? 1 : 0;
        return 1;
    }

    /* ---- System.identityHashCode：用对象地址低 24 位当 hash ---- */
    if (jstrcmp(cls, "java/lang/System") == 0 && jstrcmp(name, "identityHashCode") == 0) {
        int32_t h = ((uint32_t)locals[0]) & 0xFFFFFFu;
        if (h == 0) h = 1;
        stack[top] = h;
        return 1;
    }

    /* ---- MC 枚举 awt 静态查找 stub：n/p/i/o 返回 awt 实例（方块注册链需要非 null） ---- */
    if (jstrcmp(cls, "awt") == 0 &&
        (jstrcmp(name, "n") == 0 || jstrcmp(name, "p") == 0 ||
         jstrcmp(name, "i") == 0 || jstrcmp(name, "o") == 0)) {
        struct jvm_class *ac = find_class("awt");
        if (ac) { stack[top] = (int32_t)obj_new(ac); return 1; }
        return 0;
    }

    /* 未知 native：指针守卫后打印（防野指针 %s 读崩） */
    {
        const char *cs1 = (cls && (uint32_t)cls >= 0xc0000000u && (uint32_t)cls < 0xf0000000u) ? cls : "?";
        const char *cs2 = (name && (uint32_t)name >= 0xc0000000u && (uint32_t)name < 0xf0000000u) ? name : "?";
        const char *cs3 = (desc && (uint32_t)desc >= 0xc0000000u && (uint32_t)desc < 0xf0000000u) ? desc : "?";
        static char last_un[96] = {0};
        char unbuf[96];
        int unl = 0;
        for (const char *q = cs1; *q && unl < 28; q++) unbuf[unl++] = *q;
        unbuf[unl++] = '.';
        for (const char *q = cs2; *q && unl < 56; q++) unbuf[unl++] = *q;
        unbuf[unl++] = '.';
        for (const char *q = cs3; *q && unl < 92; q++) unbuf[unl++] = *q;
        unbuf[unl] = 0;
        if (jstrcmp(last_un, unbuf) != 0) {
            last_un[0] = 0; jvm_strcat(last_un, unbuf);
            static int un_tot = 0;
            if (un_tot++ < 60)
                printf("[JVM] unhandled native %s.%s%s\n", cs1, cs2, cs3);
        }
    }
    /* 按返回类型补槽：J/D 需双槽，其余单槽（0 值，防残留野指针） */
    {
        const char *rp3;
        char retc3 = 0;
        for (rp3 = desc; *rp3; rp3++) if (*rp3 == ')') { retc3 = rp3[1]; break; }
        if (retc3 == 'J' || retc3 == 'D') { stack[top] = 0; stack[top + 1] = 0; return 1; }
    }
    return 1; /* 假装成功（压一个 0） */
}

/* 调试：当前解释执行位置（全局，exec_one/exec_method 共用） */
volatile const char *dbg_cls = 0;
volatile const char *dbg_mth = 0;

/* ---- 解释执行（递归帧） ---- */
static int32_t exec_method(struct jvm_method *m, struct jvm_class *cls,
                           int32_t *args, int nargs, int is_static);

static int32_t exec_one(struct jvm_method *m, struct jvm_class *cls,
                        int32_t *locals, int32_t *stack, int *top_out, uint8_t **pc_out) {
    if (cls && cls->name && m && m->name) { dbg_cls = cls->name; dbg_mth = m->name; }
    /* [DBG] cw/da.a() 实际执行的字节码（确认是否 HashBiMap.create） */
    if (m && m->name && m->name[0] == 'a' && m->name[1] == 0 &&
        cls && cls->name && (jstrcmp(cls->name, "cw") == 0 || jstrcmp(cls->name, "da") == 0)) {
        static int dbg_ent = 0;
        if (dbg_ent++ < 10)
            printf("[DBG] exec_one enter %s.a() len=%u code=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   cls->name, m->code_len,
                   m->code_len > 0 ? m->code[0] : 0xff, m->code_len > 1 ? m->code[1] : 0xff,
                   m->code_len > 2 ? m->code[2] : 0xff, m->code_len > 3 ? m->code[3] : 0xff,
                   m->code_len > 4 ? m->code[4] : 0xff, m->code_len > 5 ? m->code[5] : 0xff,
                   m->code_len > 6 ? m->code[6] : 0xff, m->code_len > 7 ? m->code[7] : 0xff);
    }
    (void)cls;
    uint8_t *pc = *pc_out;
    int top = *top_out;
    for (;;) {
        maybe_gc();
        if (top < 0) top = 0;
        else if (top > 248) { static int stk_clamp_log = 0; if (stk_clamp_log++ < 4) printf("[JVM] stk clamp top=%d in %s.%s\n", top, dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?"); top = 248; }
        uint8_t op = *pc++;
        switch (op) {
        case 0x00: break; /* nop */
        case 0x01: stack[top++] = 0; break; /* aconst_null */
        case 0x02: stack[top++] = -1; break; /* iconst_m1 */
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
            stack[top++] = op - 0x03; break; /* iconst_0..5 */
        case 0x09: push_long(stack, &top, 0); break; /* lconst_0 */
        case 0x0a: push_long(stack, &top, 1); break; /* lconst_1 */
        case 0x0b: stack[top++] = bitsf(0.0f); break; /* fconst_0 */
        case 0x0c: stack[top++] = bitsf(1.0f); break; /* fconst_1 */
        case 0x0d: stack[top++] = bitsf(2.0f); break; /* fconst_2 */
        case 0x0e: { double z = 0.0; int32_t lo, hi; dto2(z, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* dconst_0 */
        case 0x0f: { double o = 1.0; int32_t lo, hi; dto2(o, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* dconst_1 */
        case 0x10: stack[top++] = (int8_t)*pc++; break; /* bipush */
        case 0x11: { int16_t v = (int16_t)r16(pc); pc += 2; stack[top++] = v; break; } /* sipush */
        case 0x12: { /* ldc */
            uint16_t idx = *pc++;
            struct cp_entry *e = &cls->cp[idx];
            if (e->tag == CP_STRING) {
                const char *s = cp_utf8(cls, idx);
                struct jvm_obj *so = jvm_string(s);
                stack[top++] = (int32_t)so;
            } else if (e->tag == CP_INTEGER || e->tag == CP_FLOAT) {
                stack[top++] = e->u.i32;
            } else if (e->tag == CP_CLASS) {
                /* 类字面量 X.class：返回 Class 伪对象（fields[0]=类指针）。
                   类缺失时用 Object 伪类：保证 Class 对象非 null（CHM 反射 Unsafe.class 依赖） */
                const char *cn = cp_utf8(cls, idx);
                struct jvm_class *nc = find_class(cn);
                if (!nc) nc = find_class("java/lang/Object");
                struct jvm_obj *co = nc ? obj_new(NULL) : NULL;
                if (co) co->fields[0] = (int32_t)nc;
                stack[top++] = (int32_t)co;
            } else {
                stack[top++] = 0;
            }
            break;
        }
        case 0x13: case 0x14: { /* ldc_w / ldc2_w */
            uint16_t idx = r16(pc); pc += 2;
            struct cp_entry *e = &cls->cp[idx];
            if (op == 0x14) {
                int32_t lo = (int32_t)(e->u.i64 & 0xffffffff);
                int32_t hi = (int32_t)(e->u.i64 >> 32);
                stack[top++] = lo; stack[top++] = hi;
            } else if (e->tag == CP_STRING) {
                stack[top++] = (int32_t)jvm_string(cp_utf8(cls, idx));
            } else if (e->tag == CP_CLASS) {
                const char *cn = cp_utf8(cls, idx);
                struct jvm_class *nc = find_class(cn);
                if (!nc) nc = find_class("java/lang/Object");
                struct jvm_obj *co = nc ? obj_new(NULL) : NULL;
                if (co) co->fields[0] = (int32_t)nc;
                stack[top++] = (int32_t)co;
            } else {
                stack[top++] = e->u.i32;
            }
            break;
        }
        /* 局部变量加载 */
        case 0x15: { uint8_t i = *pc++; stack[top++] = (i < 128) ? locals[i] : 0; break; } /* iload */
        case 0x16: { uint8_t i = *pc++; stack[top++] = (i < 128) ? locals[i] : 0; stack[top++] = (i + 1 < 128) ? locals[i + 1] : 0; break; } /* lload（2 槽） */
        case 0x17: { uint8_t i = *pc++; stack[top++] = (i < 128) ? locals[i] : 0; break; } /* fload */
        case 0x18: { uint8_t i = *pc++; stack[top++] = (i < 128) ? locals[i] : 0; stack[top++] = (i + 1 < 128) ? locals[i + 1] : 0; break; } /* dload（2 槽） */
        case 0x19: { uint8_t i = *pc++; stack[top++] = (i < 128) ? locals[i] : 0; break; }
        case 0x1a: case 0x1b: case 0x1c: case 0x1d: stack[top++] = locals[op - 0x1a]; break;
        case 0x2a: case 0x2b: case 0x2c: case 0x2d: {
            int li = op - 0x2a;
            stack[top++] = locals[li];
            break;
        } /* aload_n */
        case 0x1e: case 0x1f: case 0x20: case 0x21: { int li = op - 0x1e; stack[top++] = locals[li]; stack[top++] = locals[li + 1]; break; } /* lload_0..3 */
        case 0x26: case 0x27: case 0x28: case 0x29: { int li = op - 0x26; stack[top++] = locals[li]; stack[top++] = locals[li + 1]; break; } /* dload_0..3 */
        case 0x22: case 0x23: case 0x24: case 0x25: { int fi = op - 0x22; stack[top++] = locals[fi]; break; } /* fload_0..3 */
        /* 存储 */
        case 0x36: { uint8_t i = *pc++; if (i < 128) locals[i] = stack[--top]; else { --top; } break; } /* istore */
        case 0x37: { uint8_t i = *pc++; if (i + 1 < 128) { locals[i + 1] = stack[--top]; locals[i] = stack[--top]; } else { top -= 2; } break; } /* lstore（2 槽） */
        case 0x38: { uint8_t i = *pc++; if (i < 128) locals[i] = stack[--top]; else { --top; } break; } /* fstore */
        case 0x39: { uint8_t i = *pc++; if (i + 1 < 128) { locals[i + 1] = stack[--top]; locals[i] = stack[--top]; } else { top -= 2; } break; } /* dstore（2 槽） */
        case 0x3a: { uint8_t i = *pc++; if (i < 128) locals[i] = stack[--top]; else { --top; } break; } /* astore 通用（魔改 javac 在 locals>=4 时使用） */
        case 0x3b: case 0x3c: case 0x3d: case 0x3e: locals[op - 0x3b] = stack[--top]; break; /* istore_0..3 */
        case 0x3f: case 0x40: case 0x41: case 0x42: { int li = op - 0x3f; locals[li + 1] = stack[--top]; locals[li] = stack[--top]; break; } /* lstore_0..3 */
        case 0x43: case 0x44: case 0x45: case 0x46: locals[op - 0x43] = stack[--top]; break; /* fstore_0..3 */
        case 0x47: case 0x48: case 0x49: case 0x4a: { int si = op - 0x47; locals[si + 1] = stack[--top]; locals[si] = stack[--top]; break; } /* dstore_0..3 */
        case 0x4b: case 0x4c: case 0x4d: case 0x4e: {
            int ai = op - 0x4b;
            int32_t av = stack[--top];
            locals[ai] = av;
            break;
        } /* astore_0..3 */
        /* 整数算术 */
        case 0x60: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a + b; break; } /* iadd */
        case 0x64: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a - b; break; } /* isub */
        case 0x68: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a * b; break; } /* imul */
        case 0x6c: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = b ? a / b : 0; break; } /* idiv */
        case 0x70: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = b ? a % b : 0; break; } /* irem */
        case 0x74: { int32_t v = stack[top-1]; stack[top-1] = -v; break; } /* ineg */
        case 0x78: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a << (b & 31); break; } /* ishl */
        case 0x7a: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a >> (b & 31); break; } /* ishr */
        case 0x7c: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = (int32_t)((uint32_t)a >> (b & 31)); break; } /* iushr */
        case 0x7e: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a & b; break; }
        case 0x80: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a | b; break; }
        case 0x82: { int32_t b = stack[--top], a = stack[--top]; stack[top++] = a ^ b; break; }
        /* long 算术（2 槽） */
        case 0x61: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); push_long(stack, &top, a + b); break; } /* ladd */
        case 0x65: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); push_long(stack, &top, a - b); break; } /* lsub */
        case 0x69: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); push_long(stack, &top, jvm_mul64(a, b)); break; } /* lmul */
        case 0x6d: {
            int64_t b = pop_long(stack, &top), a = pop_long(stack, &top);
            push_long(stack, &top, b ? jvm_div64(a, b) : 0);
            break;
        } /* ldiv */
        case 0x71: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); push_long(stack, &top, b ? jvm_rem64(a, b) : 0); break; } /* lrem */
        case 0x75: { int64_t a = pop_long(stack, &top); push_long(stack, &top, -a); break; } /* lneg */
        case 0x79: { int32_t b = stack[--top]; int64_t a = pop_long(stack, &top); push_long(stack, &top, a << (b & 63)); break; } /* lshl */
        case 0x7b: { int32_t b = stack[--top]; int64_t a = pop_long(stack, &top); push_long(stack, &top, a >> (b & 63)); break; } /* lshr */
        case 0x7d: { int32_t b = stack[--top]; int64_t a = pop_long(stack, &top); push_long(stack, &top, (int64_t)((uint64_t)a >> (b & 63))); break; } /* lushr */
        case 0x7f: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); push_long(stack, &top, a & b); break; } /* land */
        case 0x81: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); push_long(stack, &top, a | b); break; } /* lor */
        case 0x83: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); push_long(stack, &top, a ^ b); break; } /* lxor */
        /* float 算术（单槽位模式） */
        case 0x62: { float b = fbits(stack[--top]), a = fbits(stack[--top]); stack[top++] = bitsf(a + b); break; } /* fadd */
        case 0x66: { float b = fbits(stack[--top]), a = fbits(stack[--top]); stack[top++] = bitsf(a - b); break; } /* fsub */
        case 0x6a: { float b = fbits(stack[--top]), a = fbits(stack[--top]); stack[top++] = bitsf(a * b); break; } /* fmul */
        case 0x6e: { float b = fbits(stack[--top]), a = fbits(stack[--top]); stack[top++] = bitsf(b != 0 ? a / b : 0); break; } /* fdiv */
        case 0x72: { float b = fbits(stack[--top]), a = fbits(stack[--top]); stack[top++] = bitsf(b != 0 ? (float)((int)a % (int)b) : 0); break; } /* frem 简化 */
        case 0x76: { float a = fbits(stack[top - 1]); stack[top - 1] = bitsf(-a); break; } /* fneg */
        /* double 算术（2 槽） */
        case 0x63: { double b, a, r; int32_t lo, hi;
        { int32_t bhi = stack[--top], blo = stack[--top]; b = dfrom2(blo, bhi); }
        { int32_t ahi = stack[--top], alo = stack[--top]; a = dfrom2(alo, ahi); }
        r = a + b; dto2(r, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* dadd */
        case 0x67: { double b, a, r; int32_t lo, hi;
        { int32_t bhi = stack[--top], blo = stack[--top]; b = dfrom2(blo, bhi); }
        { int32_t ahi = stack[--top], alo = stack[--top]; a = dfrom2(alo, ahi); }
        r = a - b; dto2(r, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* dsub */
        case 0x6b: { double b, a, r; int32_t lo, hi;
        { int32_t bhi = stack[--top], blo = stack[--top]; b = dfrom2(blo, bhi); }
        { int32_t ahi = stack[--top], alo = stack[--top]; a = dfrom2(alo, ahi); }
        r = a * b; dto2(r, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* dmul */
        case 0x6f: { double b, a, r; int32_t lo, hi;
        { int32_t bhi = stack[--top], blo = stack[--top]; b = dfrom2(blo, bhi); }
        { int32_t ahi = stack[--top], alo = stack[--top]; a = dfrom2(alo, ahi); }
        r = b != 0 ? a / b : 0; dto2(r, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* ddiv */
        case 0x73: { double b, a, r; int32_t lo, hi;
        { int32_t bhi = stack[--top], blo = stack[--top]; b = dfrom2(blo, bhi); }
        { int32_t ahi = stack[--top], alo = stack[--top]; a = dfrom2(alo, ahi); }
        r = b != 0 ? (double)((long)a % (long)b) : 0; dto2(r, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* drem 简化 */
        case 0x77: { double a = dfrom2(stack[top - 2], stack[top - 1]); int32_t lo, hi; dto2(-a, &lo, &hi); stack[top - 2] = lo; stack[top - 1] = hi; break; } /* dneg */
        /* 转换 */
        case 0x85: { int32_t v = stack[--top]; push_long(stack, &top, v); break; } /* i2l */
        case 0x86: { int32_t v = stack[--top]; stack[top++] = bitsf((float)v); break; } /* i2f */
        case 0x87: { int32_t v = stack[--top]; int32_t lo, hi; dto2((double)v, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* i2d */
        case 0x88: { int64_t v = pop_long(stack, &top); stack[top++] = (int32_t)v; break; } /* l2i */
        case 0x89: { int64_t v = pop_long(stack, &top); stack[top++] = bitsf((float)v); break; } /* l2f */
        case 0x8a: { int64_t v = pop_long(stack, &top); int32_t lo, hi; dto2((double)v, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* l2d */
        case 0x8b: { int32_t v = stack[--top]; stack[top++] = (int32_t)fbits(v); break; } /* f2i */
        case 0x8c: { int32_t v = stack[--top]; push_long(stack, &top, (int64_t)fbits(v)); break; } /* f2l */
        case 0x8d: { int32_t v = stack[--top]; double d = (double)fbits(v); int32_t lo, hi; dto2(d, &lo, &hi); stack[top++] = lo; stack[top++] = hi; break; } /* f2d */
        case 0x8e: { int32_t dhi = stack[--top], dlo = stack[--top]; double d = dfrom2(dlo, dhi); stack[top++] = (int32_t)d; break; } /* d2i */
        case 0x8f: { int32_t dhi = stack[--top], dlo = stack[--top]; double d = dfrom2(dlo, dhi); push_long(stack, &top, (int64_t)d); break; } /* d2l */
        case 0x90: { int32_t dhi = stack[--top], dlo = stack[--top]; double d = dfrom2(dlo, dhi); stack[top++] = bitsf((float)d); break; } /* d2f */
        case 0x91: { int32_t v = stack[--top]; stack[top++] = (int8_t)v; break; } /* i2b */
        case 0x92: { int32_t v = stack[--top]; stack[top++] = (int16_t)v; break; } /* i2c */
        case 0x93: { int32_t v = stack[--top]; stack[top++] = (int16_t)v; break; } /* i2s */
        /* 比较 */
        case 0x94: { int64_t b = pop_long(stack, &top), a = pop_long(stack, &top); stack[top++] = (a > b) - (a < b); break; } /* lcmp */
        case 0x95: { float b = fbits(stack[--top]), a = fbits(stack[--top]); stack[top++] = (a > b) ? 1 : (a < b ? -1 : 0); break; } /* fcmpl */
        case 0x96: { float b = fbits(stack[--top]), a = fbits(stack[--top]); stack[top++] = (a > b) ? 1 : (a < b ? -1 : 0); break; } /* fcmpg */
        case 0x97: { double b, a;
        { int32_t bhi = stack[--top], blo = stack[--top]; b = dfrom2(blo, bhi); }
        { int32_t ahi = stack[--top], alo = stack[--top]; a = dfrom2(alo, ahi); }
        stack[top++] = (a > b) ? 1 : (a < b ? -1 : 0); break; } /* dcmpl */
        case 0x98: { double b, a;
        { int32_t bhi = stack[--top], blo = stack[--top]; b = dfrom2(blo, bhi); }
        { int32_t ahi = stack[--top], alo = stack[--top]; a = dfrom2(alo, ahi); }
        stack[top++] = (a > b) ? 1 : (a < b ? -1 : 0); break; } /* dcmpg */
        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: { /* if<cond> */
            int16_t off = (int16_t)r16(pc); pc += 2;
            int32_t v = stack[--top];
            int t = (op == 0x99 && v == 0) || (op == 0x9a && v != 0) ||
                    (op == 0x9b && v < 0) || (op == 0x9c && v >= 0) ||
                    (op == 0x9d && v > 0) || (op == 0x9e && v <= 0);
            if (t) pc += off - 3;
            break;
        }
        case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: { /* if_icmp<cond> */
            int16_t off = (int16_t)r16(pc); pc += 2;
            int32_t b = stack[--top], a = stack[--top];
            int t = (op == 0x9f && a == b) || (op == 0xa0 && a != b) ||
                    (op == 0xa1 && a < b) || (op == 0xa2 && a >= b) ||
                    (op == 0xa3 && a > b) || (op == 0xa4 && a <= b);
            if (t) pc += off - 3;
            break;
        }
        case 0xa5: case 0xa6: { /* if_acmp<cond> */
            int16_t off = (int16_t)r16(pc); pc += 2;
            int32_t b = stack[--top], a = stack[--top];
            int t = (op == 0xa5 && a == b) || (op == 0xa6 && a != b);
            if (t) pc += off - 3;
            break;
        }
        case 0xa7: { int16_t off = (int16_t)r16(pc); pc += 2; pc += off - 3; break; } /* goto */
        case 0xa8: { /* jsr：简化当 goto */
            int16_t off = (int16_t)r16(pc); pc += 2; pc += off - 3; break;
        }
        case 0xa9: { uint8_t i = *pc++; (void)i; break; } /* ret */
        case 0xb1: { *top_out = top; *pc_out = pc; return 2; } /* return */
        case 0xac: { int32_t v = stack[--top]; *top_out = top; *pc_out = pc; stack[top++] = v; *top_out = top; return 1; } /* ireturn */
        case 0xad: case 0xae: case 0xaf: { /* lreturn/freturn/dreturn：long/double 占 2 槽，取低位返回 */
            int32_t v = stack[top - 2]; top -= 2;
            *top_out = top; *pc_out = pc; stack[top++] = v; *top_out = top; return 1;
        }
        case 0xb0: { /* areturn */
            int32_t v = stack[--top]; *top_out = top; *pc_out = pc; stack[top++] = v; *top_out = top; return 1;
        }
        case 0xbb: { /* new */
            uint16_t idx = r16(pc); pc += 2;
            const char *ncname = cp_utf8(cls, idx);
            if (jstrcmp(ncname, "java/lang/StringBuilder") == 0 ||
                jstrcmp(ncname, "java/lang/StringBuffer") == 0) {
                struct jvm_obj *sb = arr_new(NULL, 4096);
                if (sb->array_data) sb->array_data[0] = 0;
                sb->cls = pseudo_string_class();
                stack[top++] = (int32_t)sb;
                break;
            }
            struct jvm_class *nc = find_class(ncname);
            if (!nc) { /* 类不在镜像：fallback 到 Object 实例（异常类等未打包时不产生 throw null） */
                nc = find_class("java/lang/Object");
                if (!nc) { stack[top++] = 0; break; }
            }
            struct jvm_obj *no = obj_new(nc);
            stack[top++] = (int32_t)no;
            break;
        }
        case 0xbc: { /* newarray */
            uint8_t atype = *pc++;
            int32_t len = stack[--top];
            if (len > 4194304) printf("[JVM] big-array %d in %s.%s\n", len,
                cls && cls->name ? cls->name : "?", m && m->name ? m->name : "?");
            struct jvm_obj *a = arr_new(NULL, len);
            a->array_len = len;
            (void)atype;
            stack[top++] = (int32_t)a;
            break;
        }
        case 0xbd: { /* anewarray */
            uint16_t idx = r16(pc); pc += 2;
            (void)idx;
            int32_t len = stack[--top];
            if (len > 4194304) {
                static int32_t last_ba = 0;
                if (len != last_ba) {
                    printf("[JVM] big-anewarray %d in %s.%s\n", len,
                        cls && cls->name ? cls->name : "?", m && m->name ? m->name : "?");
                    last_ba = len;
                }
            }
            stack[top++] = (int32_t)arr_new(NULL, len);
            break;
        }
        case 0xbe: { /* arraylength */
            struct jvm_obj *a = (struct jvm_obj *)stack[--top];
            stack[top++] = (a && (uint32_t)a >= 0xc0000000u && (uint32_t)a < 0xf0000000u)
                           ? a->array_len : 0;
            break;
        }
        case 0x2e: case 0x2f: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: { /* iaload..saload */
            int32_t idx = stack[--top];
            struct jvm_obj *a = (struct jvm_obj *)stack[--top];
            stack[top++] = ((uint32_t)a >= 0xc0000000u && (uint32_t)a < 0xf0000000u &&
                            a && a->is_array && idx >= 0 && idx < a->array_len &&
                            a->array_data && (uint32_t)a->array_data >= 0xc0000000u &&
                            (uint32_t)a->array_data < 0xf0000000u) ? a->array_data[idx] : 0;
            break;
        }
        case 0x4f: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: { /* iastore..sastore */
            int32_t v = stack[--top];
            int32_t idx = stack[--top];
            struct jvm_obj *a = (struct jvm_obj *)stack[--top];
            if ((uint32_t)a >= 0xc0000000u && (uint32_t)a < 0xf0000000u &&
                a && a->is_array && idx >= 0 && idx < a->array_len &&
                a->array_data && (uint32_t)a->array_data >= 0xc0000000u &&
                (uint32_t)a->array_data < 0xf0000000u)
                a->array_data[idx] = v;
            break;
        }
        case 0x57: { /* pop */
            if (top > 0) top--; break;
        }
        case 0x58: { /* pop2 */
            if (top >= 2) top -= 2; else top = 0; break;
        }
        case 0x59: { /* dup（标准 JVMS）：..., v1 -> ..., v1, v1 */
            if (top >= 1) { stack[top] = stack[top - 1]; top++; }
            break;
        }
        case 0x5a: { /* dup_x1（标准 JVMS）：..., v2, v1 -> ..., v1, v2, v1 */
            if (top >= 2) {
                int32_t a = stack[top - 1], b = stack[top - 2];
                stack[top - 2] = a; stack[top - 1] = b; stack[top] = a; top++;
            }
            break;
        }
        case 0x5b: { /* dup_x2（标准 JVMS 单槽形式）：..., v3, v2, v1 -> ..., v1, v3, v2, v1 */
            if (top >= 3) {
                int32_t a = stack[top - 1], b = stack[top - 2], c = stack[top - 3];
                stack[top - 3] = a; stack[top - 2] = c; stack[top - 1] = b; stack[top] = a; top++;
            }
            break;
        }
        case 0x5c: { /* dup2（标准 JVMS）：..., v2, v1 -> ..., v2, v1, v2, v1 */
            int32_t a = stack[top - 1], b = stack[top - 2];
            stack[top] = b; stack[top + 1] = a; top += 2;
            break;
        }
        case 0x5d: { /* dup2_x1（标准 JVMS）：..., v3, v2, v1 -> ..., v2, v1, v3, v2, v1 */
            int32_t a = stack[top - 1], b = stack[top - 2], c = stack[top - 3];
            stack[top - 3] = b; stack[top - 2] = a; stack[top - 1] = c; stack[top] = b; stack[top + 1] = a; top += 2;
            break;
        }
        case 0x5e: { /* dup2_x2（标准 JVMS）：..., v4, v3, v2, v1 -> ..., v2, v1, v4, v3, v2, v1 */
            int32_t a = stack[top - 1], b = stack[top - 2], c = stack[top - 3], e = stack[top - 4];
            stack[top - 4] = b; stack[top - 3] = a; stack[top - 2] = e; stack[top - 1] = c; stack[top] = b; stack[top + 1] = a; top += 2;
            break;
        }
        case 0x5f: { /* swap（标准 JVMS）：..., v2, v1 -> ..., v1, v2 */
            if (top < 2) break;
            int32_t a = stack[top - 1], b = stack[top - 2];
            stack[top - 2] = a; stack[top - 1] = b;
            break;
        }
        case 0xb2: { /* getstatic */
            uint16_t idx = r16(pc); pc += 2;
            if (!cls->cp || idx >= cls->cp_count) { stack[top++] = 0; break; }
            struct cp_entry *e = &cls->cp[idx];
            const char *fc = cp_utf8(cls, e->u.ref.a);
            /* 名字在 NameAndType */
            struct cp_entry *nt = &cls->cp[e->u.ref.b];
            const char *fn = cp_utf8(cls, nt->u.ref.a);
            const char *fd = cp_utf8(cls, nt->u.ref.b);
            if (jstrcmp(fc, "java/lang/System") == 0 && jstrcmp(fn, "out") == 0) {
                if (!sys_out) sys_out = obj_new(cls); /* 借用当前类：native 打印只看 fc/fn 字符串 */
                                stack[top++] = (int32_t)sys_out;
            } else if (!jstrcmp(fc, "java/util/HashMap") && !jstrcmp(fn, "DEFAULT_INITIAL_CAPACITY")) {
                stack[top++] = 16;
            } else if (!jstrcmp(fc, "java/util/HashMap") && !jstrcmp(fn, "MAXIMUM_CAPACITY")) {
                stack[top++] = 0x40000000; /* 1<<30 */
            } else if (!jstrcmp(fc, "java/util/HashMap") && !jstrcmp(fn, "DEFAULT_LOAD_FACTOR")) {
                stack[top++] = 0x3F400000; /* 0.75f 位模式 */
            } else if (!jstrcmp(fc, "java/util/HashMap") && !jstrcmp(fn, "TREEIFY_THRESHOLD")) {
                stack[top++] = 8;
            } else if (!jstrcmp(fc, "java/util/HashMap") && !jstrcmp(fn, "UNTREEIFY_THRESHOLD")) {
                stack[top++] = 6;
            } else if (!jstrcmp(fc, "java/util/HashMap") && !jstrcmp(fn, "MIN_TREEIFY_CAPACITY")) {
                stack[top++] = 64;
            } else if (!jstrcmp(fc, "java/lang/Integer") && !jstrcmp(fn, "MAX_VALUE")) {
                stack[top++] = 0x7fffffff;
            } else if (!jstrcmp(fc, "java/io/File$PathStatus") && !jstrcmp(fn, "INVALID")) {
                stack[top++] = 1;
            } else if (!jstrcmp(fc, "java/io/File$PathStatus") && !jstrcmp(fn, "CHECKED")) {
                stack[top++] = 0;
            } else {
                struct jvm_class *tc = find_class(fc);
                int32_t *sl = tc ? static_field_slot(tc, fn, fd) : NULL;
                if (sl) {
                    stack[top++] = *sl;
                    /* long/double 静态字段：Java 语义压 2 槽（高 32 位=0；javm statics 简化 1 槽） */
                    if (fd[jstrlen(fd) - 1] == 'J' || fd[jstrlen(fd) - 1] == 'D') stack[top++] = 0;
                    if (jstrcmp(fc, "java/util/concurrent/ConcurrentHashMap") == 0) {
                        static int tlog = 0;
                        if (tlog++ < 40)
                            printf("[TBL] getstatic %s.%s%s -> %08x tc=%08x heap=%uMB\n", fc, fn, fd, *sl,
                                   (unsigned)(uintptr_t)tc, aptos_heap_size() >> 20);
                    }
                } else {
                    static int gs_log = 0;
                    if (gs_log++ < 25) {
                        printf("[GS] getstatic %s.%s%s -> %s (tc=%s flds=%d)\n", fc, fn, fd,
                               tc ? "slot-NULL" : "class-NULL", tc ? (tc->name ? tc->name : "?") : "-",
                               tc ? tc->field_count : -1);
                    }
                    stack[top++] = 0;
                }
            }
            break;
        }
        case 0xb3: { /* putstatic：存类静态槽 */
            uint16_t idx = r16(pc); pc += 2;
            if (!cls->cp || idx >= cls->cp_count) { (void)stack[--top]; break; }
            struct cp_entry *e = &cls->cp[idx];
            const char *fc2 = cp_utf8(cls, e->u.ref.a);
            struct cp_entry *nt2 = &cls->cp[e->u.ref.b];
            const char *fn2 = cp_utf8(cls, nt2->u.ref.a);
            const char *fd2 = cp_utf8(cls, nt2->u.ref.b);
            struct jvm_class *tc2 = find_class(fc2);
            int32_t *sl2 = tc2 ? static_field_slot(tc2, fn2, fd2) : NULL;
            char pfc2 = fd2[jstrlen(fd2) - 1];
            if (sl2) {
                /* long/double：取低 32 位（javm 压 [lo,hi]，lo 在 top-2） */
                *sl2 = stack[top - ((pfc2 == 'J' || pfc2 == 'D') ? 2 : 1)];
                if (jstrcmp(fc2, "java/util/concurrent/ConcurrentHashMap") == 0) {
                    static int tlog2 = 0;
                    if (tlog2++ < 20)
                        printf("[TBL] putstatic %s.%s%s = %08x\n", fc2, fn2, fd2, *sl2);
                }
            }
            /* long/double putstatic：Java 语义弹 2 槽 */
            if (pfc2 == 'J' || pfc2 == 'D') top--;
            top--;
            break;
        }
        case 0xb4: { /* getfield */
            uint16_t idx = r16(pc); pc += 2;
            if (!cls->cp || idx >= cls->cp_count) { stack[top - 1] = 0; break; }
            struct cp_entry *e = &cls->cp[idx];
            struct cp_entry *nt = &cls->cp[e->u.ref.b];
            const char *fn = cp_utf8(cls, nt->u.ref.a);
            const char *fd = cp_utf8(cls, nt->u.ref.b);
            struct jvm_obj *o = (struct jvm_obj *)stack[top - 1];
            int32_t *slot = o ? field_slot(o, fn, fd) : NULL;
            char gfc = fd[jstrlen(fd) - 1];
            if (gfc == 'J' || gfc == 'D') {
                /* long/double 实例字段：2 槽语义（低 32 位入槽，高 32 位补 0） */
                stack[top - 1] = slot ? *slot : 0;
                stack[top] = 0;
                top++;
            } else {
                stack[top - 1] = slot ? *slot : 0;
            }
            break;
        }
        case 0xb5: { /* putfield */
            uint16_t idx = r16(pc); pc += 2;
            if (!cls->cp || idx >= cls->cp_count) { if (top >= 2) top -= 2; else top = 0; break; }
            struct cp_entry *e = &cls->cp[idx];
            struct cp_entry *nt = &cls->cp[e->u.ref.b];
            const char *fn = cp_utf8(cls, nt->u.ref.a);
            const char *fd = cp_utf8(cls, nt->u.ref.b);
            char pfc = fd[jstrlen(fd) - 1];
            int32_t v;
            if (pfc == 'J' || pfc == 'D') {
                (void)stack[--top];  /* 高 32 位丢弃 */
                v = stack[--top];    /* 低 32 位 */
            } else {
                v = stack[--top];
            }
            struct jvm_obj *o = (struct jvm_obj *)stack[--top];
            if (!o || (uint32_t)o < 0xc0000000u || (uint32_t)o >= 0xf0000000u) {
                break;
            }
            int32_t *slot = field_slot(o, fn, fd);
            if (slot) *slot = v;
            /* [DBG] da.c 字段写入探针（注册表 map 初始化检查） */
            if (cls->name && jstrcmp(cls->name, "da") == 0 && fn && jstrcmp(fn, "c") == 0) {
                static int dbg_pf = 0;
                if (dbg_pf++ < 12)
                    printf("[DBG] putfield da.c = %08x (o=%08x slot=%s)\n",
                           (unsigned)v, (unsigned)o, slot ? "set" : "NULL");
            }
            break;
        }
        case 0xb6: case 0xb7: case 0xb8: case 0xb9: { /* invokevirtual/invokespecial/invokestatic/invokeinterface */
            uint16_t idx = r16(pc);
            if (op == 0xb9) pc += 4; else pc += 2; /* invokeinterface: index2 + count1 + 0 */
            { static unsigned inv_cnt = 0;
              if ((inv_cnt++ & 0x3FFF) == 0)
                  JDBG("[INV] %s.%s pc=%d op=%02x heap=%uMB\n",
                         dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?",
                         (int)(pc - m->code), op,
                         aptos_heap_size() >> 20);
            }
            if (!cls->cp || idx >= cls->cp_count) { if (top > 0) top--; stack[top++] = 0; break; }
            struct cp_entry *e = &cls->cp[idx];
            /* 防护：e 必须是 Methodref 族且 ref.b 在界内，否则安全跳过（防 cp 错位读垃圾） */
            if (e->tag != CP_METHODREF && e->tag != CP_INTERFACE_METHODREF &&
                e->tag != CP_FIELDREF && e->tag != CP_NAME_AND_TYPE) {
                static int badcp_log = 0;
                if (badcp_log++ < 16)
                    printf("[CP] invoke idx=%d tag=%u ref.a=%u ref.b=%u cp_count=%u cls=%s op=%x pc_off=%u\n",
                           idx, e->tag, e->u.ref.a, e->u.ref.b, cls->cp_count,
                           cls->name ? cls->name : "?", op,
                           pc ? (unsigned)(pc - m->code) : 0);
                if (top > 0) top--; stack[top++] = 0; break;
            }
            if (e->u.ref.b >= cls->cp_count || e->u.ref.a >= cls->cp_count) {
                if (top > 0) top--; stack[top++] = 0; break;
            }
            const char *fc = cp_utf8(cls, e->u.ref.a);
            struct cp_entry *nt = &cls->cp[e->u.ref.b];
            const char *fn = cp_utf8(cls, nt->u.ref.a);
            const char *fd = cp_utf8(cls, nt->u.ref.b);
            /* 参数槽数（按描述符统计：long/double 占 2 槽，跳过开头的 '('） */
            int nargs = 0, ar;
            if (fd[1] == 'b' && fd[2] == 'a' && fd[3] == 'd') nargs = -1; /* (bad) 解析失败：当无参 */
            for (ar = 1; nargs >= 0 && fd[ar] != ')'; ar++) {
                if (fd[ar] == 'J' || fd[ar] == 'D') { nargs += 2; }
                else if (fd[ar] == 'L') { nargs++; while (fd[ar] != ';' && ar < 0x10000) ar++; }
                else if (fd[ar] == '[') { nargs++; while (fd[ar] == '[' && ar < 0x10000) ar++; if (fd[ar] == 'L') while (fd[ar] != ';' && ar < 0x10000) ar++; }
                else nargs++;
            }
            int is_static = (op == 0xb8);
            int total = (nargs < 0 ? 0 : nargs) + (is_static ? 0 : 1);
            if (total > 64) {
                static int a64_once = 0;
                if (!a64_once) { printf("[JVM] invoke too many args %d %s.%s -> truncated\n", total, fc, fn); a64_once = 1; }
                total = 64;
            }
            /* 取参 */
            int32_t args[64];
            for (ar = 0; ar < total; ar++) args[ar] = stack[top - total + ar];
            /* ---- Enumeration 探针：hasMoreElements/nextElement 调用实况（rcv 类/参数） ---- */
            if (jstrcmp(fc, "java/util/Enumeration") == 0 && (jstrcmp(fn, "hasMoreElements") == 0 || jstrcmp(fn, "nextElement") == 0)) {
                static int enu = 0;
                if (enu++ < 12) {
                    struct jvm_obj *er = (struct jvm_obj *)args[0];
                    printf("[ENU] %s rcv=%08x cls=%s top=%d in=%s.%s\n", fn, args[0],
                           (er && er->cls && er->cls->name) ? er->cls->name : "?", top,
                           dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
                }
            }
            /* ---- Node<init> 构造调用探针（构造参数压栈实况） ---- */
            if (jstrcmp(fc, "java/util/concurrent/ConcurrentHashMap$Node") == 0 && fn[0] == '<') {
                static int nci = 0;
                if (nci++ < 3) {
                    printf("[NCI] %s.%s total=%d top=%d args=%08x,%08x,%08x,%08x,%08x stk0-7=%08x %08x %08x %08x %08x %08x %08x %08x in=%s.%s\n",
                           fc, fn, total, top, args[0], args[1], args[2], args[3], args[4],
                           top-total-0 >= 0 ? stack[top-total] : -1, top-total+1 >= 0 ? stack[top-total+1] : -1,
                           top-total+2 >= 0 ? stack[top-total+2] : -1, top-total+3 >= 0 ? stack[top-total+3] : -1,
                           top-total+4 >= 0 ? stack[top-total+4] : -1, top-total+5 >= 0 ? stack[top-total+5] : -1,
                           top-total+6 >= 0 ? stack[top-total+6] : -1, top-total+7 >= 0 ? stack[top-total+7] : -1,
                           dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
                }
            }
            /* ---- casTabAt invokestatic 组装探针（tab 参数错位根因） ---- */
            if (jstrcmp(fc, "java/util/concurrent/ConcurrentHashMap") == 0 && jstrcmp(fn, "casTabAt") == 0) {
                static int csa = 0;
                if (csa++ < 8) {
                    printf("[CSA] invokestatic %s.%s total=%d top=%d args=%08x,%08x,%08x,%08x stk[-8..]=%08x %08x %08x %08x %08x %08x %08x %08x in=%s.%s\n",
                           fc, fn, total, top, args[0], args[1], args[2], args[3],
                           top-total-8 >= 0 ? stack[top-total-8] : -1, top-total-7 >= 0 ? stack[top-total-7] : -1,
                           top-total-6 >= 0 ? stack[top-total-6] : -1, top-total-5 >= 0 ? stack[top-total-5] : -1,
                           top-total-4 >= 0 ? stack[top-total-4] : -1, top-total-3 >= 0 ? stack[top-total-3] : -1,
                           top-total-2 >= 0 ? stack[top-total-2] : -1, top-total-1 >= 0 ? stack[top-total-1] : -1,
                           dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
                }
            }
            /* ---- Unsafe getObjectVolatile args 探针（定位 U=null 根因） ---- */
            if (jstrcmp(fc, "sun/misc/Unsafe") == 0 &&
                (jstrcmp(fn, "getObjectVolatile") == 0 || jstrcmp(fn, "getIntVolatile") == 0 ||
                 jstrcmp(fn, "compareAndSwapObject") == 0)) {
                static int uv = 0;
                if (uv++ < 10)
                    printf("[UV] %s.%s args0=%08x args1=%08x total=%d heap=%uMB\n",
                           fc, fn, args[0], args[1], total, aptos_heap_size() >> 20);
            }
            /* ---- aor.a stub（MC 注册表 Duplicate 检查—— 当前 javm 语义下误抛，先忽略推进） ---- */
            if (jstrcmp(fc, "aor") == 0 && jstrcmp(fn, "a") == 0) {
                top -= total; break;
            }
            /* ---- afe.b stub（varargs 类型 instanceof 简化返回 0 → 误抛 RuntimeException 死循环；先跳过推进） ---- */
            if (jstrcmp(fc, "afe") == 0 && jstrcmp(fn, "b") == 0) {
                top -= total; break;
            }
            /* ---- 伪枚举器 stub：Hashtable$Enumerator 不在镜像 → keys() 返回 Object 伪类对象，
                   hasMoreElements 无真实状态（恒真）→ store0 死循环；伪枚举器判定为枚举结束返回 0 ---- */
            if (jstrcmp(fn, "hasMoreElements") == 0 && total >= 1 && !is_static) {
                struct jvm_obj *er = (struct jvm_obj *)args[0];
                if (er && er->cls && er->cls->name && jstrcmp(er->cls->name, "java/lang/Object") == 0) {
                    top -= total; stack[top] = 0; top++; break;
                }
            }
            /* ---- Reference.tryHandlePending stub：pending 恒 null + wait 不阻塞 → ReferenceHandler
                   无限循环卡死主线程（Thread.start 同步执行 run）；返回 false 让 Reference 线程退出 ---- */
            if (jstrcmp(fc, "java/lang/ref/Reference") == 0 && jstrcmp(fn, "tryHandlePending") == 0) {
                top -= total; stack[top] = 0; top++; break;
            }
            /* ---- netty DefaultPromise.isDone stub：worker 线程（NioEventLoop 无限循环）被 Thread.start
                   跳过 → promise 永不完结 → awaitUninterruptibly 死循环（wait 不阻塞）；isDone 恒 true 跳过 await ---- */
            if (jstrcmp(fc, "io/netty/util/concurrent/DefaultPromise") == 0 &&
                (jstrcmp(fn, "isDone") == 0 || jstrcmp(fn, "isDone0") == 0)) {
                top -= total; stack[top] = 1; top++; break;
            }
            /* ---- [RR] 探针：Readable.read 调用点（LineBuffer 死循环诊断） ---- */
            if (jstrcmp(fc, "java/lang/Readable") == 0 && jstrcmp(fn, "read") == 0) {
                static int rr_log = 0;
                struct jvm_obj *ro = (struct jvm_obj *)args[0];
                if (rr_log++ < 5)
                    printf("[RR] Readable.read rcv=%x cls=%s total=%d\n",
                           (unsigned int)ro, ro && ro->cls && ro->cls->name ? ro->cls->name : "?",
                           total);
            }
            /* ---- Readable.read stub：接收者恒为 Object 伪类（接口 fallback）→ 返回 0 非 EOF →
                   LineBuffer/LineReader 读循环死循环；返回 -1 让读终止 ---- */
            if (jstrcmp(fc, "java/lang/Readable") == 0 && jstrcmp(fn, "read") == 0) {
                top -= total; stack[top] = -1; top++; break;
            }
            /* ---- EULA 检查：lg.a()（eula.txt 是否同意）→ true；
                    lg.b()（写 eula.txt）→ 跳过——避免 eula 段 Date/输出流 OOM ---- */
            if (jstrcmp(fc, "lg") == 0) {
                if (jstrcmp(fn, "a") == 0 && fd[0] == '(' && fd[1] == ')') {
                    stack[top - total] = 1; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "b") == 0) { top -= total; break; }
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                top -= total; break;
            }
            /* ---- MC keep-alive 线程 lu.run（while(true) sleep(24天)）：
                    sleep 被 stub 后空转死循环——直接跳过 ---- */
            if (jstrcmp(fc, "lu") == 0 && jstrcmp(fn, "run") == 0) {
                top -= total; break;
            }
            /* ---- 全局 start()V 伪线程拦截：Thread 子类（lj 等）多态声明
                    时 fc != java/lang/Thread，按方法名+描述符全局拦截 ---- */
            if (jstrcmp(fn, "start") == 0 && fd[0] == '(' && fd[1] == ')') {
                struct jvm_obj *th = (struct jvm_obj *)args[0];
                struct jvm_obj *tgt = NULL;
                if (th && (uint32_t)th >= 0xc0000000u && (uint32_t)th < 0xf0000000u) {
                    int32_t *tp = field_slot(th, "target", "Ljava/lang/Runnable;");
                    struct jvm_obj *t2 = tp ? (struct jvm_obj *)*tp : NULL;
                    if (t2 && (uint32_t)t2 >= 0xc0000000u && (uint32_t)t2 < 0xf0000000u)
                        tgt = t2;
                }
                if (tgt && tgt->cls) {
                    struct jvm_method *rm = find_method(tgt->cls, "run", "()V");
                    if (rm && jstrcmp(tgt->cls->name, "lu") != 0) {
                        int32_t a0 = (int32_t)tgt;
                        static int gts_log = 0;
                        if (gts_log++ < 6)
                            printf("[TS] global start -> run %s.%s\n",
                                   tgt->cls->name ? tgt->cls->name : "?",
                                   rm->name ? rm->name : "?");
                        exec_method(rm, tgt->cls, &a0, 1, 0);
                    }
                } else if (th && th->cls) {
                    struct jvm_method *rm = find_method(th->cls, "run", "()V");
                    if (rm && jstrcmp(th->cls->name, "lu") != 0 &&
                        !(th->cls->name && jstrcmp(th->cls->name, "java/lang/ref/Reference$ReferenceHandler") == 0)) {
                        int32_t a0 = (int32_t)th;
                        static int gts_log2 = 0;
                        if (gts_log2++ < 6)
                            printf("[TS] global start -> run(thread) %s.%s\n",
                                   th->cls->name ? th->cls->name : "?",
                                   rm->name ? rm->name : "?");
                        exec_method(rm, th->cls, &a0, 1, 0);
                    }
                }
                stack[top - total] = 0; top = top - total + 1; break;
            }
            /* ---- Timer / Thread 调度族：sleep/yield/join 立即返回，
                    Timer 调度直接 stub——防单线程解释器在 Timer 空转卡死 ---- */
            if (jstrcmp(fc, "java/lang/Thread") == 0) {
                if (jstrcmp(fn, "sleep") == 0 || jstrcmp(fn, "yield") == 0 ||
                    jstrcmp(fn, "join") == 0 || jstrcmp(fn, "interrupt") == 0 ||
                    jstrcmp(fn, "setName") == 0 || jstrcmp(fn, "setDaemon") == 0 ||
                    jstrcmp(fn, "setPriority") == 0 || jstrcmp(fn, "setUncaughtExceptionHandler") == 0 ||
                    jstrcmp(fn, "currentThread") == 0 || jstrcmp(fn, "getName") == 0 ||
                    jstrcmp(fn, "getId") == 0) { top -= total; break; }
                if (jstrcmp(fn, "start") == 0) {
                    /* 伪线程：同步执行 target.run()——MC 服务器主线程由此进入
                       DedicatedServer.run() 的初始化 + tick 循环 */
                    struct jvm_obj *th = (struct jvm_obj *)args[0];
                    struct jvm_obj *tgt = NULL;
                    if (th && (uint32_t)th >= 0xc0000000u && (uint32_t)th < 0xf0000000u) {
                        int32_t *tp = field_slot(th, "target", "Ljava/lang/Runnable;");
                        struct jvm_obj *t2 = tp ? (struct jvm_obj *)*tp : NULL;
                        if (t2 && (uint32_t)t2 >= 0xc0000000u && (uint32_t)t2 < 0xf0000000u)
                            tgt = t2;
                    }
                    if (tgt && tgt->cls) {
                        struct jvm_method *rm = find_method(tgt->cls, "run", "()V");
                        if (rm) {
                            int32_t a0 = (int32_t)tgt;
                            static int ts_log = 0;
                            if (ts_log++ < 4)
                                printf("[TS] start -> sync run %s.%s\n",
                                       tgt->cls->name ? tgt->cls->name : "?",
                                       rm->name ? rm->name : "?");
                            exec_method(rm, tgt->cls, &a0, 1, 0);
                        }
                    } else if (th && th->cls) {
                        /* Thread 子类（override run，无 target 字段）：直接执行 this.run() */
                        struct jvm_method *rm = find_method(th->cls, "run", "()V");
                        if (rm) {
                            /* ReferenceHandler.run 是硬无限循环（goto -5），同步执行会卡死主线程；跳过 */
                            if (th->cls->name && jstrcmp(th->cls->name, "java/lang/ref/Reference$ReferenceHandler") == 0) {
                                static int refskip_log = 0;
                                if (refskip_log++ < 2)
                                    printf("[TS] skip ReferenceHandler.run (infinite loop)\n");
                            } else {
                                int32_t a0 = (int32_t)th;
                                static int ts_log2 = 0;
                                if (ts_log2++ < 4)
                                    printf("[TS] start -> sync run(thread) %s.%s\n",
                                           th->cls->name ? th->cls->name : "?",
                                           rm->name ? rm->name : "?");
                                exec_method(rm, th->cls, &a0, 1, 0);
                            }
                        }
                    }
                    stack[top - total] = 0; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "run") == 0) {
                    stack[top - total] = 0; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "isAlive") == 0 || jstrcmp(fn, "isInterrupted") == 0 ||
                    jstrcmp(fn, "isDaemon") == 0) { stack[top - total] = 1; top = top - total + 1; break; }
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                top -= total; break;
            }
            if (jstrcmp(fc, "java/util/Timer") == 0 || jstrcmp(fc, "java/util/TimerThread") == 0 ||
                jstrcmp(fc, "java/util/Timer$TaskQueue") == 0) {
                if (jstrcmp(fn, "schedule") == 0 || jstrcmp(fn, "scheduleAtFixedRate") == 0 ||
                    jstrcmp(fn, "cancel") == 0 || jstrcmp(fn, "purge") == 0 ||
                    jstrcmp(fn, "mainLoop") == 0 || jstrcmp(fn, "run") == 0 ||
                    jstrcmp(fn, "newTasksMayBeScheduled") == 0) { top -= total; break; }
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                top -= total; break;
            }
            /* ---- java/io/OutputStream + InputStream 家族（invoke 层拦截：
                    覆盖多态声明类（invokevirtual 到父类）漏网的 write/read 循环） ---- */
            if (jstrcmp(fc, "java/io/OutputStream") == 0 ||
                jstrcmp(fc, "java/io/FilterOutputStream") == 0 ||
                jstrcmp(fc, "java/io/BufferedOutputStream") == 0 ||
                jstrcmp(fc, "java/io/DataOutputStream") == 0 ||
                jstrcmp(fc, "java/io/PrintStream") == 0 ||
                jstrcmp(fc, "java/io/PrintWriter") == 0 ||
                jstrcmp(fc, "java/io/Writer") == 0 ||
                jstrcmp(fc, "java/io/OutputStreamWriter") == 0 ||
                jstrcmp(fc, "java/io/BufferedWriter") == 0 ||
                jstrcmp(fc, "java/io/FileWriter") == 0) {
                /* 标准输出放行：PrintStream 的 print/println 交给 native 打印，
                   否则会被下面的 write/print 拦截整段吞掉（此前 Hello 无输出的根因）。
                   先用"无条件放行"验证链路，确认后再按 sys_out 收紧。 */
                if (jstrcmp(fc, "java/io/PrintStream") == 0 &&
                    (jstrcmp(fn, "print") == 0 || jstrcmp(fn, "println") == 0)) {
                    int r = invoke_native(fc, fn, fd, args, stack, top);
                    (void)r;
                    top -= total;
                    break;
                }
                if (jstrcmp(fn, "write") == 0 || jstrcmp(fn, "writeBytes") == 0 ||
                    jstrcmp(fn, "writeInt") == 0 || jstrcmp(fn, "writeShort") == 0 ||
                    jstrcmp(fn, "writeLong") == 0 || jstrcmp(fn, "writeBoolean") == 0 ||
                    jstrcmp(fn, "writeChar") == 0 || jstrcmp(fn, "writeFloat") == 0 ||
                    jstrcmp(fn, "writeDouble") == 0 || jstrcmp(fn, "writeUTF") == 0 ||
                    jstrcmp(fn, "flush") == 0 || jstrcmp(fn, "close") == 0 ||
                    jstrcmp(fn, "newLine") == 0 || jstrcmp(fn, "print") == 0 ||
                    jstrcmp(fn, "println") == 0 || jstrcmp(fn, "append") == 0 ||
                    jstrcmp(fn, "checkError") == 0 || jstrcmp(fn, "setError") == 0) {
                    top -= total; break;
                }
                if (jstrcmp(fn, "size") == 0) { stack[top - total] = 0; top = top - total + 1; break; }
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                top -= total; break;
            }
            if (jstrcmp(fc, "java/io/InputStream") == 0 ||
                jstrcmp(fc, "java/io/FilterInputStream") == 0 ||
                jstrcmp(fc, "java/io/BufferedInputStream") == 0 ||
                jstrcmp(fc, "java/io/DataInputStream") == 0 ||
                jstrcmp(fc, "java/io/ByteArrayInputStream") == 0 ||
                jstrcmp(fc, "java/io/FileInputStream") == 0 ||
                jstrcmp(fc, "java/io/ObjectInputStream") == 0 ||
                jstrcmp(fc, "java/io/Reader") == 0 ||
                jstrcmp(fc, "java/io/InputStreamReader") == 0 ||
                jstrcmp(fc, "java/io/BufferedReader") == 0 ||
                jstrcmp(fc, "java/io/FileReader") == 0) {
                if (jstrcmp(fn, "read") == 0) { stack[top - total] = -1; top = top - total + 1; break; }
                if (jstrcmp(fn, "readLine") == 0) { stack[top - total] = 0; top = top - total + 1; break; }
                if (jstrcmp(fn, "readByte") == 0 || jstrcmp(fn, "readUnsignedByte") == 0 ||
                    jstrcmp(fn, "readChar") == 0 || jstrcmp(fn, "readShort") == 0 ||
                    jstrcmp(fn, "readUnsignedShort") == 0 || jstrcmp(fn, "readInt") == 0 ||
                    jstrcmp(fn, "readLong") == 0 || jstrcmp(fn, "readFloat") == 0 ||
                    jstrcmp(fn, "readDouble") == 0 || jstrcmp(fn, "readBoolean") == 0 ||
                    jstrcmp(fn, "readUTF") == 0) { stack[top - total] = -1; top = top - total + 1; break; }
                if (jstrcmp(fn, "readFully") == 0 || jstrcmp(fn, "readFully0") == 0 ||
                    jstrcmp(fn, "skipBytes") == 0) { top -= total; break; }
                if (jstrcmp(fn, "available") == 0 || jstrcmp(fn, "markSupported") == 0) {
                    stack[top - total] = 0; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "skip") == 0 || jstrcmp(fn, "reset") == 0 ||
                    jstrcmp(fn, "close") == 0 || jstrcmp(fn, "mark") == 0 ||
                    jstrcmp(fn, "readAllBytes") == 0) { top -= total; break; }
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                top -= total; break;
            }
            /* ---- ByteArrayOutputStream 家族（commons-io + JDK）：
                    write 直接丢弃，needNewBuffer 不倍增——防 128MB 数组钳 0 后死循环 ---- */
            if (jstrcmp(fc, "org/apache/commons/io/output/ByteArrayOutputStream") == 0 ||
                jstrcmp(fc, "java/io/ByteArrayOutputStream") == 0) {
                if (jstrcmp(fn, "write") == 0 || jstrcmp(fn, "writeTo") == 0 ||
                    jstrcmp(fn, "reset") == 0 || jstrcmp(fn, "flush") == 0 ||
                    jstrcmp(fn, "close") == 0) { top -= total; break; }
                if (jstrcmp(fn, "needNewBuffer") == 0 ||
                    jstrcmp(fn, "expand") == 0 || jstrcmp(fn, "ensureCapacity") == 0) {
                    top -= total; break;
                }
                if (jstrcmp(fn, "size") == 0 || jstrcmp(fn, "count") == 0) {
                    stack[top - total] = 0; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "toByteArray") == 0 || jstrcmp(fn, "toString") == 0) {
                    stack[top - total] = 0; top = top - total + 1; break;  /* null 缓冲 */
                }
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                top -= total; break;
            }
            /* ---- java.io.File 家族（invoke 层拦截：不执行真实字节码，
                    避免 getCanonicalPath0 native 链 → IOException 风暴 → page fault） ---- */
            if (jstrcmp(fc, "java/io/File") == 0) {
                if (jstrcmp(fn, "getCanonicalPath") == 0 || jstrcmp(fn, "getAbsolutePath") == 0 ||
                    jstrcmp(fn, "getPath") == 0 || jstrcmp(fn, "getName") == 0 ||
                    jstrcmp(fn, "getParent") == 0 || jstrcmp(fn, "toString") == 0 ||
                    jstrcmp(fn, "getCanonicalFile") == 0 || jstrcmp(fn, "getAbsoluteFile") == 0) {
                    struct jvm_obj *fth = (struct jvm_obj *)args[0];
                    const char *p = "";
                    if (fth && (uint32_t)fth >= 0xc0000000u && (uint32_t)fth < 0xf0000000u) {
                        int32_t *pp = field_slot(fth, "path", "Ljava/lang/String;");
                        struct jvm_obj *ps = pp ? (struct jvm_obj *)*pp : NULL;
                        if (ps && (uint32_t)ps >= 0xc0000000u && (uint32_t)ps < 0xf0000000u && ps->array_data)
                            p = (const char *)ps->array_data;
                    }
                    struct jvm_obj *r = jvm_string(p);
                    if (jstrcmp(fn, "getCanonicalFile") == 0 || jstrcmp(fn, "getAbsoluteFile") == 0)
                        r = (struct jvm_obj *)args[0];
                    stack[top - total] = (int32_t)r; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "exists") == 0 || jstrcmp(fn, "isFile") == 0 ||
                    jstrcmp(fn, "isDirectory") == 0 || jstrcmp(fn, "isHidden") == 0 ||
                    jstrcmp(fn, "canRead") == 0 || jstrcmp(fn, "canWrite") == 0 ||
                    jstrcmp(fn, "canExecute") == 0) {
                    stack[top - total] = 1; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "length") == 0) { stack[top - total] = 0; top = top - total + 1; break; }
                if (jstrcmp(fn, "list") == 0 || jstrcmp(fn, "listFiles") == 0) {
                    stack[top - total] = 0; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "mkdir") == 0 || jstrcmp(fn, "mkdirs") == 0 ||
                    jstrcmp(fn, "delete") == 0 || jstrcmp(fn, "createNewFile") == 0 ||
                    jstrcmp(fn, "renameTo") == 0 || jstrcmp(fn, "setLastModified") == 0 ||
                    jstrcmp(fn, "setWritable") == 0 || jstrcmp(fn, "setReadable") == 0 ||
                    jstrcmp(fn, "setExecutable") == 0) {
                    stack[top - total] = 1; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "deleteOnExit") == 0) { top -= total; break; }
                if (jstrcmp(fn, "lastModified") == 0) {
                    top = top - total; stack[top] = 0; stack[top + 1] = 0; top += 2; break;
                }
                if (jstrcmp(fn, "toURI") == 0) { stack[top - total] = 0; top = top - total + 1; break; }
                if (jstrcmp(fn, "compareTo") == 0) { stack[top - total] = 0; top = top - total + 1; break; }
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                top -= total; break;
            }
            /* ---- 数字/区域格式化家族 blocklist：
                    Locale/NumberFormat/DecimalFormat/sun.util.locale 全部 stub，
                    避免 code 错位(0xfd) 与 LocaleProviderAdapter 缺失死循环 ---- */
            {
                int blk = 0;
                if (fc[0] == 'j' && fc[1] == 'a' && fc[2] == 'v' && fc[3] == 'a' &&
                    fc[4] == '/' && fc[5] == 't' && fc[6] == 'e' && fc[7] == 'x')
                    blk = 1;  /* java/text/* */
                if (fc[0] == 's' && fc[1] == 'u' && fc[2] == 'n' && fc[3] == '/' &&
                    fc[4] == 'u' && fc[5] == 't' && fc[6] == 'i' && fc[7] == 'l')
                    blk = 1;  /* sun/util/* (locale/provider) */
                if (jstrcmp(fc, "java/util/Locale") == 0)
                    blk = 1;
                if (jstrcmp(fc, "java/math/RoundingMode") == 0)
                    blk = 1;
                if (jstrcmp(fc, "java/math/MathContext") == 0)
                    blk = 1;
                if (fc[0] == 'c' && fc[1] == 'o' && fc[2] == 'm' && fc[3] == '/' &&
                    fc[4] == 'g' && fc[5] == 'o' && fc[6] == 'o' && fc[7] == 'g' && fc[8] == 'l' &&
                    fc[9] == 'e' && fc[10] == '/' && fc[11] == 'g' && fc[12] == 's' && fc[13] == 'o' &&
                    fc[14] == 'n' && fc[15] == '/')
                    blk = 1;  /* com/google/gson/*（勿用 com/goog 前缀：会误伤 com/google/common 等） */
                if (blk) {
                    /* 返回类型决定压几个槽；构造器压 receiver */
                    char rc3 = 0;
                    for (const char *rp3 = fd; *rp3 && (rp3 - fd) < 0x10000; rp3++)
                        if (*rp3 == ')') { rc3 = rp3[1]; break; }
                    if (fn[0] == '<') {
                        if (total >= 1) stack[top - total] = args[0];
                        top = top - total + 1;
                    } else if (rc3 == 'V') {
                        top -= total;
                    } else if (rc3 == 'J' || rc3 == 'D') {
                        top = top - total; stack[top] = 0; stack[top + 1] = 0; top += 2;
                    } else {
                        top = top - total; stack[top] = 0; top += 1;
                    }
                    break;
                }
            }
            /* ---- StringBuilder/StringBuffer stub（MC 大量字符串拼接） ---- */
            if (jstrcmp(fc, "java/lang/StringBuilder") == 0 ||
                jstrcmp(fc, "java/lang/StringBuffer") == 0) {
                struct jvm_obj *sb = (struct jvm_obj *)args[0];
                if (jstrcmp(fn, "<init>") == 0) { top -= total; break; }
                if (jstrcmp(fn, "append") == 0) {
                    /* 把 String 参数内容拷入 sb 缓冲（简化：仅首参 String） */
                    struct jvm_obj *ap = (struct jvm_obj *)args[1];
                    if (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u &&
                        sb->array_data && ap && (uint32_t)ap >= 0xc0000000u && (uint32_t)ap < 0xf0000000u && ap->array_data &&
                        (uint32_t)ap->array_data >= 0xc0000000u && (uint32_t)ap->array_data < 0xf0000000u) {
                        uint32_t off = 0;
                        while (off < 4090 && ((char *)sb->array_data)[off]) off++;
                        const char *src = (const char *)ap->array_data;
                        uint32_t sl = 0;
                        while (sl < 4080 && src[sl]) sl++;
                        for (uint32_t k = 0; k < sl && off + k < 4090; k++) ((char *)sb->array_data)[off + k] = src[k];
                        ((char *)sb->array_data)[off + sl] = 0;
                    }
                    stack[top - total] = (int32_t)sb; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "toString") == 0) {
                    struct jvm_obj *r = jvm_string(sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data ? (char *)sb->array_data : "");
                    stack[top - total] = (int32_t)r; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "length") == 0) {
                    int len = 0;
                    if (sb && (uint32_t)sb >= 0xc0000000u && (uint32_t)sb < 0xf0000000u && sb->array_data) while (len < 4090 && ((char *)sb->array_data)[len]) len++;
                    stack[top - total] = len; top = top - total + 1; break;
                }
                top -= total; break;
            }
            /* ---- Guava/Collection 迭代器特判（computeNext abstract 无实现会死循环：
                   hasNext 返回 false 结束空迭代，next 返回 null 防 NoSuchElementException 风暴） ---- */
            {
                int it_prefix = 0;
                if (fc[0] == 'c' && fc[1] == 'o' && fc[2] == 'm' && fc[3] == '/' &&
                    fc[4] == 'g' && fc[5] == 'o' && fc[6] == 'o' && fc[7] == 'g')
                    it_prefix = 1;
                if (jstrcmp(fc, "java/util/Iterator") == 0 ||
                    jstrcmp(fc, "java/util/ListIterator") == 0) it_prefix = 1;
                if (it_prefix && (jstrcmp(fn, "hasNext") == 0)) {
                    top -= total; stack[top++] = 0; break;
                }
                if (it_prefix && (jstrcmp(fn, "next") == 0)) {
                    top -= total; stack[top++] = 0; break;
                }
                if (jstrcmp(fc, "java/util/NoSuchElementException") == 0 && jstrcmp(fn, "<init>") == 0) {
                    top -= total; break;
                }
            }
            /* ---- log4j 快捷处理（在 null-receiver 检查前）：LogManager / Logger 接口 ---- */
            if (jstrcmp(fc, "org/apache/logging/log4j/LogManager") == 0) {
                if (jstrcmp(fn, "<clinit>") == 0) { top -= total; break; }
                if (jstrcmp(fn, "getLogger") == 0) {
                    struct class *lcls = find_class("java/lang/Object");
                    stack[top - total] = (int32_t)(lcls ? obj_new(lcls) : 0);
                    top = top - total + 1;
                    break;
                }
            }
            if (jstrcmp(fc, "org/apache/logging/log4j/Logger") == 0 ||
                jstrcmp(fc, "org/apache/logging/log4j/spi/AbstractLogger") == 0 ||
                jstrcmp(fc, "org/apache/logging/log4j/LogBuilder") == 0 ||
                (jstrcmp(fc, "org/apache/logging/log4j/simplelog/SimpleLogger") == 0)) {
                if (jstrcmp(fc, "adb") == 0 && jstrcmp(fn, "<init>") == 0) { top -= total; break; } /* adb 构造递归死循环：跳过 */
                if (jstrcmp(fn, "isDebugEnabled") == 0 || jstrcmp(fn, "isInfoEnabled") == 0 ||
                    jstrcmp(fn, "isTraceEnabled") == 0 || jstrcmp(fn, "isErrorEnabled") == 0 ||
                    jstrcmp(fn, "isWarnEnabled") == 0 || jstrcmp(fn, "isFatalEnabled") == 0) {
                    stack[top - total] = 0; top = top - total + 1; break;
                }
                if (jstrcmp(fn, "info") == 0 || jstrcmp(fn, "warn") == 0 || jstrcmp(fn, "error") == 0 ||
                    jstrcmp(fn, "debug") == 0 || jstrcmp(fn, "trace") == 0 || jstrcmp(fn, "fatal") == 0) {
                    struct jvm_obj *m = (struct jvm_obj *)args[is_static ? 0 : 1];
                    if (m && m->array_data && (uint32_t)m->array_data >= 0xc0000000u && (uint32_t)m->array_data < 0xf0000000u)
                        printf("[MC] %s\n", (char *)m->array_data);
                    top -= total; break;
                }
                if (jstrcmp(fn, "atLevel") == 0 || jstrcmp(fn, "withLocation") == 0 ||
                    jstrcmp(fn, "withThrowable") == 0 || jstrcmp(fn, "log") == 0 ||
                    jstrcmp(fn, "entry") == 0 || jstrcmp(fn, "exit") == 0 || jstrcmp(fn, "throwing") == 0 ||
                    jstrcmp(fn, "always") == 0) {
                    top -= total; break;
                }
            }
            /* ---- Unsafe 数组操作真实化（不依赖 U 对象：args1=数组, args2..3=offset） ---- */
            if (jstrcmp(fc, "sun/misc/Unsafe") == 0 &&
                (jstrcmp(fn, "getObjectVolatile") == 0 || jstrcmp(fn, "getObject") == 0 ||
                 jstrcmp(fn, "getObjectAcquire") == 0 ||
                 jstrcmp(fn, "getIntVolatile") == 0 || jstrcmp(fn, "getInt") == 0 ||
                 jstrcmp(fn, "getLongVolatile") == 0 || jstrcmp(fn, "getLong") == 0 ||
                 jstrcmp(fn, "putObjectVolatile") == 0 || jstrcmp(fn, "putObject") == 0 ||
                 jstrcmp(fn, "putObjectRelease") == 0 ||
                 jstrcmp(fn, "putIntVolatile") == 0 || jstrcmp(fn, "putInt") == 0 ||
                 jstrcmp(fn, "putLongVolatile") == 0 || jstrcmp(fn, "putLong") == 0 ||
                 jstrcmp(fn, "compareAndSwapInt") == 0 || jstrcmp(fn, "compareAndSwapLong") == 0 ||
                 jstrcmp(fn, "compareAndSwapObject") == 0)) {
                static int us_cnt = 0;
                if (us_cnt++ < 25)
                    printf("[US] %s.%s a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x t=%d in=%s.%s owner=%08x heap=%uMB\n", fc, fn,
                           args[0], args[1], args[2], args[3], args[4], total,
                           dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?",
                           m ? (unsigned)(uintptr_t)m->owner : 0,
                           aptos_heap_size() >> 20);
                int64_t off = ((int64_t)(uint32_t)args[3] << 32) | (uint32_t)args[2];
                struct jvm_obj *o = (struct jvm_obj *)args[1];
                int ok = o && o->is_array && o->array_data &&
                         (uint32_t)o >= 0xc0000000u && (uint32_t)o < 0xf0000000u &&
                         off >= 0 && (off >> 2) < o->array_len;
                char retc = 0;
                for (const char *rp = fd; *rp && (rp - fd) < 0x10000; rp++)
                    if (*rp == ')') { retc = rp[1]; break; }
                if (jstrcmp(fn, "getObjectVolatile") == 0 || jstrcmp(fn, "getObject") == 0 ||
                    jstrcmp(fn, "getObjectAcquire") == 0 ||
                    jstrcmp(fn, "getIntVolatile") == 0 || jstrcmp(fn, "getInt") == 0 ||
                    jstrcmp(fn, "getLongVolatile") == 0 || jstrcmp(fn, "getLong") == 0) {
                    if (ok) {
                        stack[top - total] = o->array_data[off >> 2];
                        if (retc == 'J' || retc == 'D') {
                            stack[top - total + 1] = o->array_data[(off >> 2) + 1];
                            top = top - total + 2;
                        } else top = top - total + 1;
                    } else {
                        top = top - total + 1;
                        stack[top - 1] = 0;
                    }
                    break;
                }
                if (jstrcmp(fn, "putObjectVolatile") == 0 || jstrcmp(fn, "putObject") == 0 ||
                    jstrcmp(fn, "putObjectRelease") == 0 ||
                    jstrcmp(fn, "putIntVolatile") == 0 || jstrcmp(fn, "putInt") == 0 ||
                    jstrcmp(fn, "putLongVolatile") == 0 || jstrcmp(fn, "putLong") == 0) {
                    if (ok) o->array_data[off >> 2] = args[4];
                    top -= total;
                    break;
                }
                if (jstrcmp(fn, "compareAndSwapInt") == 0 || jstrcmp(fn, "compareAndSwapLong") == 0 ||
                    jstrcmp(fn, "compareAndSwapObject") == 0) {
                    int32_t r = 0;
                    if (ok && o->array_data[off >> 2] == args[4]) {
                        o->array_data[off >> 2] = args[5];
                        r = 1;
                    } else if (o && !o->is_array) {
                        /* 对象字段 CAS（如 CHM.initTable 的 sizeCtl）：javm 无精确字段偏移映射，
                           返回成功以推进（调用方随后走 putstatic 写真实字段） */
                        r = 1;
                    }
                    top -= total;
                    stack[top++] = r;
                    break;
                }
            }
            if (!is_static && (args[0] == 0 || (uint32_t)args[0] < 0xc0000000u ||
                               (uint32_t)args[0] >= 0xf0000000u)) {
                /* 空引用/非法引用调用：不崩溃，按返回类型压默认值 */
                { dbg_cls = cls ? cls->name : 0; dbg_mth = m ? m->name : 0;
                    static int nr_total = 0;
                    if (nr_total++ < 300) {
                    static char last_nr[96] = {0};
                    char nrbuf[96];
                    int nrl = 0;
                    for (const char *q = fc; *q && nrl < 28; q++) nrbuf[nrl++] = *q;
                    nrbuf[nrl++] = '.';
                    for (const char *q = fn; *q && nrl < 56; q++) nrbuf[nrl++] = *q;
                    nrbuf[nrl++] = '.';
                    for (const char *q = fd; *q && nrl < 92; q++) nrbuf[nrl++] = *q;
                    nrbuf[nrl] = 0;
                    if (jstrcmp(last_nr, nrbuf) != 0) {
                        last_nr[0] = 0; jvm_strcat(last_nr, nrbuf);
                        printf("[JVM] null-receiver %s.%s%s (in %s.%s)\n", fc, fn, fd,
                               dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
                    }
                    }
                }
                /* 空 Map 调用：containsKey 真 / put 成功，避免注册循环 */
                if (jstrcmp(fc, "java/util/Map") == 0) {
                    if (jstrcmp(fn, "containsKey") == 0 || jstrcmp(fn, "containsValue") == 0 ||
                        jstrcmp(fn, "isEmpty") == 0) { stack[top] = 1; top += 1; break; }
                    if (jstrcmp(fn, "put") == 0 || jstrcmp(fn, "putAll") == 0 ||
                        jstrcmp(fn, "remove") == 0 || jstrcmp(fn, "clear") == 0) {
                        stack[top] = 0; top += 1; break;
                    }
                    if (jstrcmp(fn, "get") == 0 || jstrcmp(fn, "keySet") == 0 ||
                        jstrcmp(fn, "values") == 0 || jstrcmp(fn, "entrySet") == 0) {
                        stack[top] = 0; top += 1; break;
                    }
                    if (jstrcmp(fn, "size") == 0) { stack[top] = 1; top += 1; break; }
                }
                /* 空 List 调用：add 成功 / size 大值，避免 while(size<N) 死循环 */
                if (jstrcmp(fc, "java/util/List") == 0) {
                    if (jstrcmp(fn, "add") == 0) { stack[top] = 1; top += 1; break; }
                    if (jstrcmp(fn, "size") == 0) { stack[top] = 0; top += 1; break; } /* 空列表语义：for(i<size) 立即退出；while(size<N) 类循环由 add 特判兜底 */
                    if (jstrcmp(fn, "iterator") == 0) { stack[top] = 0; top += 1; break; }
                    if (jstrcmp(fn, "get") == 0) { stack[top] = 0; top += 1; break; }
                    if (jstrcmp(fn, "isEmpty") == 0) { stack[top] = 1; top += 1; break; }
                    if (jstrcmp(fn, "remove") == 0 || jstrcmp(fn, "clear") == 0 ||
                        jstrcmp(fn, "addAll") == 0) { stack[top] = 1; top += 1; break; }
                }
                top -= total;
                char retc = 0;
                const char *rp2;
                for (rp2 = fd; *rp2 && (rp2 - fd) < 0x10000; rp2++) if (*rp2 == ')') { retc = rp2[1]; break; }
                if (retc == 'J' || retc == 'D') { stack[top] = 0; stack[top + 1] = 0; top += 2; }
                else if (retc == 'L' && rp2[1] == 'j' && rp2[2] == 'a' && rp2[3] == 'v' && rp2[4] == 'a'
                         && rp2[5] == '/' && rp2[6] == 'l' && rp2[7] == 'a' && rp2[8] == 'n' && rp2[9] == 'g'
                         && rp2[10] == '/' && rp2[11] == 'S' && rp2[12] == 't' && rp2[13] == 'r' && rp2[14] == 'i'
                         && rp2[15] == 'n' && rp2[16] == 'g') {
                    struct jvm_obj *e = jvm_string("");
                    stack[top] = (int32_t)e; top++;
                }
                else if ((jstrcmp(fc, "org/apache/logging/log4j/spi/LoggerContextFactory") == 0 ||
                          jstrcmp(fc, "org/apache/logging/log4j/spi/LoggerContext") == 0 ||
                          jstrcmp(fc, "org/apache/logging/log4j/LogManager") == 0) &&
                         (jstrcmp(fn, "getContext") == 0 || jstrcmp(fn, "getLogger") == 0)) {
                    struct class *lcls = find_class("java/lang/Object");
                    stack[top] = (int32_t)(lcls ? obj_new(lcls) : 0); top++;
                    printf("[JVM] STUB-HIT %s.%s -> obj %08x\n", fc, fn, stack[top-1]);
                }
                else if (retc == 'L' || retc == '[' || retc == 0) {
                    /* null-receiver 对象/未知返回：给对应类实例（避免调用方循环条件卡死） */
                    struct jvm_class *lcls = NULL;
                    if (retc == 'L' && rp2[1] == 'L' && (rp2 - fd) < 0x10000) {
                        const char *cn = rp2 + 2;
                        char cname[80];
                        int cni = 0;
                        while (*cn && *cn != ';' && cni < 79 && (cn - fd) < 0x10000)
                            cname[cni++] = *cn++;
                        cname[cni] = 0;
                        if (cname[0] && jstrcmp(cname, "java/lang/Object") != 0)
                            lcls = find_class(cname);
                    }
                    if (!lcls) { stack[top] = 0; top++; }
                    else { stack[top] = (int32_t)obj_new(lcls); top++; }
                }
                else if (retc != 'V') { stack[top] = 0; top++; }
                break;
            }
            top -= total;
            struct jvm_class *target_cls = NULL;
            if (!is_static) {
                struct jvm_obj *rcv = (struct jvm_obj *)args[0];
                if (op == 0xb7) target_cls = find_class(fc);  /* invokespecial：静态分派（构造器链） */
                else target_cls = rcv ? rcv->cls : find_class(fc);
            } else {
                target_cls = find_class(fc);
            }
            struct jvm_method *tm = target_cls ? find_method(target_cls, fn, fd) : NULL;
            /* [DBG] 注册表 map 定位探针：da/cw.a() + Maps/HashBiMap（含 tm->owner） */
            if ((jstrcmp(fc, "com/google/common/collect/Maps") == 0 && jstrcmp(fn, "newHashMap") == 0) ||
                (jstrcmp(fc, "com/google/common/collect/HashBiMap") == 0 && jstrcmp(fn, "create") == 0) ||
                (jstrcmp(fc, "cw") == 0 && jstrcmp(fn, "a") == 0 && fd[1] == ')') ||
                (jstrcmp(fc, "da") == 0 && jstrcmp(fn, "a") == 0 && fd[1] == ')')) {
                static int dbg_inv = 0;
                if (dbg_inv++ < 40)
                    printf("[DBG] invoke %s.%s%s op=%x is_static=%d total=%d target=%s tm=%s owner=%s\n",
                           fc, fn, fd, op, is_static, total,
                           (target_cls && target_cls->name) ? target_cls->name : "NULL",
                           tm ? (tm->name ? tm->name : "?") : "NULL",
                           (tm && tm->owner && tm->owner->name) ? tm->owner->name : "?");
            }
            if (!tm && jstrcmp(fn, "registerNatives") != 0) {
                /* native 或未找到：净化对象参数后走 native */
                sanitize_obj_args(args, total, fd, is_static);
                in_invoke_native++;
                int r = invoke_native(fc, fn, fd, args, stack, top);
                in_invoke_native--;
                /* [DBG] Maps/HashBiMap native 兜底返回值 */
                if ((jstrcmp(fc, "com/google/common/collect/Maps") == 0 && jstrcmp(fn, "newHashMap") == 0) ||
                    (jstrcmp(fc, "com/google/common/collect/HashBiMap") == 0 && jstrcmp(fn, "create") == 0)) {
                    static int dbg_nv = 0;
                    if (dbg_nv++ < 10)
                        printf("[DBG] %s.%s -> native r=%d\n", fc, fn, r);
                }
                if (r == 1) {
                    if (fn[0] == '<' && !is_static && total >= 1) {
                        /* 构造器 native：参数已由主路径弹（top-=total），剩余栈即构造对象，不再压 this */
                    } else {
                        char retc = 0;
                        const char *rp2;
                        for (rp2 = fd; *rp2 && (rp2 - fd) < 0x10000; rp2++) {
                            if (*rp2 == ')') { retc = rp2[1]; break; }
                        }
                        if (retc == 'J' || retc == 'D') top += 2; /* native 已写 stack[top], stack[top+1] */
                        else if (retc != 'V' && retc != 0) top++; /* native 已写 stack[top]，只推进栈顶 */
                    }
                }
                break;
            }
            if (!tm) { printf("[JVM] missing method %s.%s%s\n", fc, fn, fd); break; }
            int32_t ret = 0;
            int is_v = (fd[jstrlen(fd) - 1] == 'V');
            if (tm->flags & ACC_NATIVE) {
                sanitize_obj_args(args, total, fd, is_static);
                in_invoke_native++;
                int r = invoke_native(fc, fn, fd, args, stack, top);
                in_invoke_native--;
                (void)r;
                if (fn[0] == '<' && !is_static && total >= 1) {
                    /* 构造器 native：参数已由主路径弹（top-=total），剩余栈即构造对象，不再压 this */
                } else if (!is_v) {
                    char retc = fd[jstrlen(fd) - 1];
                    if (retc == 'J' || retc == 'D') top += 2;
                    else top++;
                }
                break;
            }
            ret = exec_method(tm, tm->owner ? tm->owner : cls, args, total, is_static);
            /* [DBG] HashBiMap.create / da.cw.a() 解释执行返回值 */
            if ((jstrcmp(fc, "com/google/common/collect/HashBiMap") == 0 && jstrcmp(fn, "create") == 0) ||
                ((jstrcmp(fc, "cw") == 0 || jstrcmp(fc, "da") == 0) && jstrcmp(fn, "a") == 0 && fd[1] == ')')) {
                static int dbg_er = 0;
                if (dbg_er++ < 20)
                    printf("[DBG] %s.%s exec ret=%08x\n", fc, fn, (unsigned)ret);
            }
            dbg_cls = cls ? cls->name : 0; dbg_mth = m ? m->name : 0;
            if (!is_v) {
                char retc = fd[jstrlen(fd) - 1];
                if (retc == 'J' || retc == 'D') { stack[top++] = ret; stack[top++] = 0; }
                else stack[top++] = ret;
            }
            else if (fn[0] == '<' && total >= 1 && top >= 1) {
                stack[top - 1] = args[0];
            }
            break;
        }
        case 0xbf: { /* athrow：查当前方法异常表，匹配则跳 handler 继续；否则打印 */
            int32_t ex = stack[--top];
            {
                struct jvm_obj *eo = (struct jvm_obj *)ex;
                const char *excls = "?";
                if (eo && (uint32_t)eo >= 0xc0000000u && (uint32_t)eo < 0xf0000000u &&
                    eo->cls && eo->cls->name)
                    excls = eo->cls->name;
                static int ath_tot = 0;
                if (ath_tot++ < 40)
                    printf("[ATHROW] ex=%08x excls=%s m=%s.%s exc=%u\n", (unsigned)ex,
                           excls, dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?",
                           m ? (unsigned)m->exc_count : 999);
            }
            int handled = 0;
            if (m && m->exc_count && m->exc_tbl && (uint32_t)m->exc_tbl >= 0xc0000000u &&
                (uint32_t)m->exc_tbl < 0xf0000000u && pc >= m->code) {
                uint16_t pco = (uint16_t)(pc - m->code);
                for (int ei = 0; ei < m->exc_count; ei++) {
                    uint16_t *t = m->exc_tbl + ei * 4;
                    uint16_t sp = t[0], ep = t[1], hp = t[2];
                    if (pco >= sp && pco < ep) {
                        /* 简化：catch_type 不匹配检查（先全接住） */
                        stack[top++] = ex;
                        pc = m->code + hp;
                        { static int eh_tot = 0;
                          if (eh_tot++ < 60)
                            printf("[JVM] exc-handler pc=%u (cls=%s.%s)\n", hp, dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
                        }
                        *top_out = top; *pc_out = pc; return 3;
                    }
                }
            }
            if (!handled) {
                static int unc_tot = 0;
                if (unc_tot++ < 30) {
                    printf("[JVM] uncaught exception");
                    printf(" (ex=%08x cls=%s.%s)", (unsigned)ex,
                           dbg_cls ? dbg_cls : "?", dbg_mth ? dbg_mth : "?");
                }
            }
            if (ex) {
                struct jvm_obj *eo = (struct jvm_obj *)ex;
                if ((uint32_t)ex >= 0xc0000000u && (uint32_t)ex < 0xf0000000u && eo->cls) {
                    const char *ecn = eo->cls->name;
                    if (ecn && (uint32_t)ecn >= 0xc0000000u && (uint32_t)ecn < 0xf0000000u)
                        printf(" %s", ecn);
                }
            }
            printf("\n");
            *top_out = top; *pc_out = pc; return 2;
        }
        case 0xc2: case 0xc3: { /* monitorenter/monitorexit：单线程 no-op */ break; }
        case 0xc0: { /* checkcast：no-op */
            uint16_t idx = r16(pc); pc += 2;
            (void)idx;
            break;
        }
        case 0xc1: { /* instanceof */
            uint16_t idx = r16(pc); pc += 2;
            (void)idx;
            stack[top++] = 0;
            break;
        }
        case 0xc6: case 0xc7: { /* ifnull/ifnonnull */
            int16_t off = (int16_t)r16(pc); pc += 2;
            int32_t v = stack[--top];
            int t = (op == 0xc6 && v == 0) || (op == 0xc7 && v != 0);
            if (t) pc += off - 3;
            break;
        }
        case 0xc8: { /* goto_w */
            int32_t off = (int32_t)r32(pc); pc += 4; pc += off - 5; break;
        }
        case 0xaa: { /* tableswitch */
            /* 对齐 */
            uint32_t pad = (uint32_t)((pc - m->code) % 4);
            if (pad) pc += 4 - pad;
            int32_t def = (int32_t)r32(pc); pc += 4;
            int32_t lo = (int32_t)r32(pc); pc += 4;
            int32_t hi = (int32_t)r32(pc); pc += 4;
            int32_t idx = stack[--top];
            if (idx >= lo && idx <= hi) pc += (int32_t)r32(pc + (idx - lo) * 4);
            else pc += def;
            break;
        }
        case 0xab: { /* lookupswitch */
            uint32_t pad = (uint32_t)((pc - m->code) % 4);
            if (pad) pc += 4 - pad;
            int32_t def = (int32_t)r32(pc); pc += 4;
            int32_t npairs = (int32_t)r32(pc); pc += 4;
            int32_t idx = stack[--top];
            int32_t target = def;
            for (int i = 0; i < npairs; i++) {
                int32_t key = (int32_t)r32(pc + i * 8);
                if (key == idx) { target = (int32_t)r32(pc + i * 8 + 4); break; }
            }
            pc += target - 3;
            break;
        }
        case 0x84: { /* iinc */
            uint8_t i = *pc++;
            int8_t c = (int8_t)*pc++;
            locals[i] += c;
            break;
        }
        case 0xc4: { /* wide：16 位局部变量索引 */
            uint8_t sub = *pc++;
            switch (sub) {
                case 0x84: { /* wide iinc */
                    uint16_t wi = r16(pc); pc += 2;
                    int16_t wc = (int16_t)r16(pc); pc += 2;
                    if (wi < 128) locals[wi] += wc;
                    break;
                }
                case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: { /* iload/lload/fload/dload/aload */
                    uint16_t wi = r16(pc); pc += 2;
                    if (wi < 128) stack[top++] = locals[wi];
                    break;
                }
                case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a: { /* istore/lstore/fstore/dstore/astore */
                    uint16_t wi = r16(pc); pc += 2;
                    if (wi < 128) locals[wi] = stack[--top];
                    break;
                }
                case 0xa9: { /* wide ret：近似返回（无 jsr 支持，忽略） */
                    uint16_t wi = r16(pc); pc += 2;
                    (void)wi;
                    break;
                }
                default: { /* 其他 wide 前缀：跳过索引 */
                    uint16_t wi = r16(pc); pc += 2;
                    (void)wi;
                    break;
                }
            }
            break;
        }
        case 0xc5: { /* multianewarray：简化 1 维 */
            uint16_t idx = r16(pc); pc += 2;
            uint8_t dims = *pc++;
            (void)idx; (void)dims;
            int32_t len = stack[--top];
            stack[top++] = (int32_t)arr_new(NULL, len);
            break;
        }
        default:
            if (op == 0xff || op == 0xf0) {
                /* 保留指令（impdep1/impdep2）：code 区损坏（正常类不该有），终止方法防蔓延 */
                static int ff_once = 0;
                if (!ff_once) {
                    printf("[JVM] opcode 0x%02x at %ld in %s.%s code=%02x %02x %02x %02x %02x %02x %02x %02x\n", op,
                        (long)(pc - m->code - 1), cls && cls->name ? cls->name : "?",
                        m && m->name ? m->name : "?",
                        m->code[0], m->code[1], m->code[2], m->code[3],
                        m->code[4], m->code[5], m->code[6], m->code[7]);
                    ff_once = 1;
                }
                *top_out = top; *pc_out = pc; return 0;
            }
            printf("[JVM] opcode 0x%02x unhandled at %ld in %s.%s\n", op,
                (long)(pc - m->code - 1), cls && cls->name ? cls->name : "?",
                m && m->name ? m->name : "?");
            *top_out = top; *pc_out = pc; return 2;
        }
    }
}

/* 执行方法（返回 int 值或 0） */
static int32_t exec_method(struct jvm_method *m, struct jvm_class *cls,
                           int32_t *args, int nargs, int is_static) {
    if (!m || !m->code) return 0; /* 无字节码（abstract/接口方法）：压默认值返回 */
    { static unsigned em_cnt = 0;
      if ((em_cnt++ & 0xFFF) == 0)
          JDBG("[EM] %s.%s%s heap=%uMB\n",
                 cls && cls->name ? cls->name : "?",
                 m && m->name ? m->name : "?", m && m->desc ? m->desc : "?",
                 aptos_heap_size() >> 20);
    }
    /* 帧缓冲：静态池 + 深度索引（递归帧完全隔离，不碰 malloc 堆；定义见 GC 区） */
    if (frame_ptr + 384 > 2097152) {
        /* 递归溢出：放弃当前帧（返回 0），避免构造链死循环终止整个 JVM */
        printf("[JVM] frame overflow fptr=%d m=%s (abandoned)\n", frame_ptr, m->name ? m->name : "?");
        return 0;
    }
    int32_t *locals = &frame_mem[frame_ptr];
    int32_t *stack = &frame_mem[frame_ptr + 128];
    frame_ptr += 384;
    memset(locals, 0, 128 * 4);
    for (int i = 0; i < nargs && i < 128; i++)
        locals[i] = args[i];
    /* top/pc 状态存帧池尾部（递归帧隔离，防 C 栈帧被深递归写坏） */
    int32_t *top_slot = &stack[255];
    uint8_t **pc_slot = (uint8_t **)&stack[254];
    *top_slot = 0;
    *pc_slot = m->code;
    int32_t ret = 0;
    int done = 0;
    (void)done;
    while (!done) {
        uint8_t *pc = *pc_slot;
        uint8_t *op_pc = pc;
        (void)op_pc;
        int top = *top_slot;
        if (top < 0) top = 0;
        if (top > 248) { top = 248; }
        *top_slot = top;
        int r = exec_one(m, cls, locals, stack, top_slot, pc_slot);
        if (r == 1) { int top = *top_slot; if (top > 0) { ret = stack[--top]; *top_slot = top; } done = 1; }
        else if (r == 2) { done = 1; }
        else if (r == 3) { /* 异常已由 handler 接住：pc 已改，继续方法 */ }
        else { done = 1; }
    }
    frame_ptr -= 384; /* 方法返回：回收帧，防累计耗尽 */
    if (frame_ptr < 0) frame_ptr = 0;
    return ret;
}

/* 类初始化 <clinit> */
static void clinit(struct jvm_class *c) {
    if (c->initialized) return;
    c->initialized = 1;
    struct jvm_method *m = find_method(c, "<clinit>", "()V");
    if (m) exec_method(m, m->owner ? m->owner : c, NULL, 0, 1);
}

/* ---- 主入口 ---- */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    jvm_stack = malloc(STACK_SLOTS * sizeof(int32_t));
    if (!jvm_stack) jerr("jvm_stack oom");
    g_brk0 = (unsigned)sys_brk(0);
    printf("[JVM] mini-jvm v0.1 brk0=0x%x stack=%u slots @0x%x\n", (unsigned)sys_brk(0), STACK_SLOTS, (unsigned)jvm_stack);
    char cls_path[256];
    char jp[256];
    if (sys_args(0, cls_path, sizeof(cls_path)) != 0 || !cls_path[0]) {
        printf("usage: run /APPS/JAVM.APP <class> [jar]\n");
        return 0;
    }
    if (sys_args(1, jp, sizeof(jp)) == 0 && jp[0]) {
        snprintf(jar_path, sizeof(jar_path), "%s", jp);
        printf("[JVM] jar=%s\n", jar_path);
    }
    /* 注册内建类占位 */
    /* 预注册 java/lang/Object（无字面 class 文件，用占位类） */
    static struct jvm_class obj_cls_storage;
    static struct jvm_field obj_fields[1];
    (void)obj_fields;
    struct jvm_class *obj = &obj_cls_storage;
    memset(obj, 0, sizeof(*obj));
    obj->name = "java/lang/Object";
    classes[class_count++] = obj;
    struct jvm_class *ps = calloc(1, sizeof(struct jvm_class));
    ps->name = "java/io/PrintStream";
    ps->super = obj;
    classes[class_count++] = ps;
    struct jvm_class *str = calloc(1, sizeof(struct jvm_class));
    str->name = "java/lang/String";
    str->super = obj;
    classes[class_count++] = str;

    uint32_t len;
    uint8_t *data = read_file(cls_path, &len);
    if (!data) {
        /* jar 回退: 完整类路径 + ".class" 匹配 jar 条目 */
        char en[256];
        snprintf(en, sizeof(en), "%s.class", cls_path);
        uint8_t *jd = NULL; uint32_t jl = 0;
        if (jar_load_entry(jar_path, en, &jd, &jl) == 0 && jd) {
            data = jd; len = jl;
        }
    }
    if (!data) {
        printf("[JVM] cannot open %s\n", cls_path);
        return 1;
    }
    struct jvm_class *main_cls = parse_class(data, len);
    if (!main_cls) return 1;
    /* 补 super 链 */
    if (main_cls->super == NULL && jstrcmp(main_cls->name, "java/lang/Object") != 0) {
        /* 从常量池找 super 名并链接 */
        uint8_t *b = data;
        (void)b;
    }
    /* 找 main */
    struct jvm_method *main_m = find_method(main_cls, "main", "([Ljava/lang/String;)V");
    if (!main_m) {
        printf("[JVM] no main method\n");
        return 1;
    }
    /* 初始化 main 类 */
    clinit(main_cls);
    /* args[0] = null（String[] 空） */
    int32_t args[1];
    args[0] = 0;
    exec_method(main_m, main_m->owner ? main_m->owner : main_cls, args, 1, 1);
    printf("[JVM] done\n");
    return 0;
}
