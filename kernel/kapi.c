#include "kapi.h"
#include "heap.h"
#include "paging.h"
#include "klog.h"
#include "scheduler.h"
#include "util.h"
#include <stdarg.h>

/* ------------------------------------------------------------------ *
 *  内核堆（自研：块头 + 双向空闲链表 + first-fit + 相邻合并 + 校验）
 *
 *  不用 malloc 家族的思路，也不依赖原 bump 堆的增长语义：
 *  每块头放在载荷之前，magic 与双向指针用于检测越界写/重复释放。
 *  载荷不足时向 bump 堆再要一个 arena 段，串到链表尾部。
 * ------------------------------------------------------------------ */

#define KMEM_MAGIC     0xA110C8EDu
#define KMEM_ARENA_MIN (256u * 1024u)
/* 可释放堆单段上限：默认 1 MiB；需要给每个窗口分配显示空间时用
   -DKMEM_ARENA_MAX_KB=4096 调大（可选，默认保持原值）。 */
#ifndef KMEM_ARENA_MAX_KB
#define KMEM_ARENA_MAX_KB 1024
#endif
#define KMEM_ARENA_MAX ((u32)KMEM_ARENA_MAX_KB * 1024u)
#define KMEM_SPLIT_MIN 32u

typedef struct kmem_blk {
    u32  magic;
    u32  size;                      /* 载荷字节数 */
    u32  free;
    u32  guard;                     /* 头尾校验：0x5A5A0000 | size */
    struct kmem_blk *next;
    struct kmem_blk *prev;
} kmem_blk_t;

static kmem_blk_t *heads;           /* 所有 arena 段的链表头 */
static kmem_blk_t *tail;
static u32 arena_kb;
static u32 blocks;
static u32 inited;

#define BLK_PAYLOAD(b) ((void *)((u8 *)(b) + sizeof(kmem_blk_t)))

static u32 blk_guard(const kmem_blk_t *b)
{
    return 0x5A5A0000u | ((b->size ^ (b->size >> 16)) & 0xFFFFu);
}

/* 把一段连续内存做成一个空闲块，追加到链表 */
static void arena_add(u32 addr, u32 bytes)
{
    kmem_blk_t *b;

    if (bytes < sizeof(kmem_blk_t) + KMEM_SPLIT_MIN)
        return;
    b = (kmem_blk_t *)addr;
    b->magic = KMEM_MAGIC;
    b->size = bytes - sizeof(kmem_blk_t);
    b->free = 1;
    b->guard = blk_guard(b);
    b->next = NULL;
    b->prev = tail;
    if (tail)
        tail->next = b;
    else
        heads = b;
    tail = b;
    arena_kb += bytes / 1024;
    blocks++;
}

void kheap_init(void)
{
    u32 bytes = KMEM_ARENA_MAX;
    u32 chunk;

    if (inited)
        return;
    chunk = kmalloc(bytes);
    if (!chunk) {
        bytes = KMEM_ARENA_MIN;
        chunk = kmalloc(bytes);
    }
    if (!chunk) {
        kprintf("[kmem] no arena available, kmem_alloc will fail\n");
        inited = 1;
        return;
    }
    arena_add(chunk, bytes);
    inited = 1;
    kprintf("[kmem] heap ready: %u KiB arena, %u bytes header\n", arena_kb,
            (u32)sizeof(kmem_blk_t));
}

static kmem_blk_t *blk_prev_valid(kmem_blk_t *b)
{
    if (b->prev && b->prev->next != b)
        kpanic("kmem: block chain broken at 0x%x", (u32)b);
    if (!b->prev && heads != b)
        kpanic("kmem: head chain broken at 0x%x", (u32)b);
    return b->prev;
}

static void split_if_room(kmem_blk_t *b, u32 want)
{
    kmem_blk_t *n;
    u32 rest;

    if (b->size < want + sizeof(kmem_blk_t) + KMEM_SPLIT_MIN)
        return;
    rest = b->size - want;
    n = (kmem_blk_t *)((u8 *)BLK_PAYLOAD(b) + want);
    n->magic = KMEM_MAGIC;
    n->size = rest - sizeof(kmem_blk_t);
    n->free = 1;
    n->guard = blk_guard(n);
    n->prev = b;
    n->next = b->next;
    if (b->next)
        b->next->prev = n;
    else
        tail = n;
    b->next = n;
    b->size = want;
    b->guard = blk_guard(b);
    blocks++;
}

void *kmem_alloc(size_t size)
{
    kmem_blk_t *b;
    u32 want = (u32)size;
    u32 irq;

    if (!size)
        return NULL;
    want = (want + 7u) & ~7u;
    irq = irq_save();

    for (b = heads; b; b = b->next) {
        if (b->magic != KMEM_MAGIC)
            kpanic("kmem: bad block magic 0x%x at 0x%x", b->magic, (u32)b);
        if (!b->free || b->size < want)
            continue;
        split_if_room(b, want);
        b->free = 0;
        b->guard = blk_guard(b);
        irq_restore(irq);
        return BLK_PAYLOAD(b);
    }

    /* 无空闲块：向 bump 堆再要一段（不超过单段上限） */
    {
        u32 bytes = KMEM_ARENA_MAX;

        if (want + sizeof(kmem_blk_t) > bytes)
            bytes = (want + sizeof(kmem_blk_t) + 0xFFFu) & ~0xFFFu;
        {
            u32 chunk = kmalloc(bytes);
            if (!chunk) {
                irq_restore(irq);
                return NULL;
            }
            arena_add(chunk, bytes);
            b = tail;
            if (b && b->free && b->size >= want) {
                split_if_room(b, want);
                b->free = 0;
                b->guard = blk_guard(b);
                irq_restore(irq);
                return BLK_PAYLOAD(b);
            }
        }
    }
    irq_restore(irq);
    return NULL;
}

void *kmem_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p;

    if (n && total / n != size)
        return NULL;
    p = kmem_alloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *kmem_realloc(void *p, size_t size)
{
    kmem_blk_t *b;
    void *np;
    u32 old;

    if (!p)
        return kmem_alloc(size);
    if (!size) {
        kmem_free(p);
        return NULL;
    }
    b = (kmem_blk_t *)((u8 *)p - sizeof(kmem_blk_t));
    if (b->magic != KMEM_MAGIC || b->free || b->guard != blk_guard(b))
        kpanic("kmem: realloc on corrupted block 0x%x", (u32)p);

    old = b->size;
    if (old >= size)
        return p;
    np = kmem_alloc(size);
    if (!np)
        return NULL;
    memcpy(np, p, old);
    kmem_free(p);
    return np;
}

void kmem_free(void *p)
{
    kmem_blk_t *b;
    u32 irq;

    if (!p)
        return;
    b = (kmem_blk_t *)((u8 *)p - sizeof(kmem_blk_t));
    if (b->magic != KMEM_MAGIC)
        kpanic("kmem: free on unknown pointer 0x%x", (u32)p);
    if (b->free)
        kpanic("kmem: double free at 0x%x", (u32)p);
    if (b->guard != blk_guard(b))
        kpanic("kmem: payload overflow detected at 0x%x", (u32)p);
    blk_prev_valid(b);

    irq = irq_save();
    b->free = 1;
    /* 与后继合并 */
    if (b->next && b->next->free && b->next->magic == KMEM_MAGIC) {
        kmem_blk_t *n = b->next;
        b->size += sizeof(kmem_blk_t) + n->size;
        b->next = n->next;
        if (n->next)
            n->next->prev = b;
        else
            tail = b;
        n->magic = 0;
        blocks--;
    }
    /* 与前驱合并 */
    if (b->prev && b->prev->free && b->prev->magic == KMEM_MAGIC) {
        kmem_blk_t *pr = b->prev;
        pr->size += sizeof(kmem_blk_t) + b->size;
        pr->next = b->next;
        if (b->next)
            b->next->prev = pr;
        else
            tail = pr;
        b->magic = 0;
        blocks--;
    }
    b->guard = blk_guard(b);
    irq_restore(irq);
}

void kheap_stats(u32 *arena, u32 *used, u32 *blks)
{
    kmem_blk_t *b;
    u32 u = 0;

    for (b = heads; b; b = b->next)
        if (!b->free)
            u += b->size;
    if (arena)
        *arena = arena_kb;
    if (used)
        *used = u / 1024;
    if (blks)
        *blks = blocks;
}

/* 启动自检：分配/写/释放/复用/邻居完整性，任何一项错就 panic */
void kheap_selftest(void)
{
    u8 *a, *b, *c;
    u32 i;

    if (!heads)
        return;
    a = (u8 *)kmem_alloc(96);
    b = (u8 *)kmem_alloc(200);
    c = (u8 *)kmem_alloc(320);
    if (!a || !b || !c)
        kpanic("kmem selftest: allocation failed");
    for (i = 0; i < 96; i++)  a[i] = 0xAA;
    for (i = 0; i < 200; i++) b[i] = 0xBB;
    for (i = 0; i < 320; i++) c[i] = 0xCC;

    kmem_free(b);
    b = (u8 *)kmem_alloc(180);      /* 应复用刚释放的块（或切出空闲块） */
    if (!b)
        kpanic("kmem selftest: reuse failed");
    for (i = 0; i < 180; i++) b[i] = 0xDD;

    for (i = 0; i < 96; i++)
        if (a[i] != 0xAA)
            kpanic("kmem selftest: neighbor block A corrupted at %u", i);
    for (i = 0; i < 320; i++)
        if (c[i] != 0xCC)
            kpanic("kmem selftest: neighbor block C corrupted at %u", i);

    kmem_free(a);
    kmem_free(b);
    kmem_free(c);
    kprintf("[kmem] selftest ok (alloc/free/merge/guard)\n");
}

/* ------------------------------------------------------------------ *
 *  panic / 断言
 * ------------------------------------------------------------------ */
void kpanic(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    __asm__ volatile("cli");
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("\n[PANIC] %s\n", buf);
    kprintf("[PANIC] task %d, system halted\n", scheduler_cur_id());
    for (;;)
        __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ *
 *  用户指针安全访问
 * ------------------------------------------------------------------ */
int user_ok(u32 uptr, u32 len)
{
    if (!len)
        return 1;
    if (uptr + len < uptr)          /* 加法溢出 */
        return 0;
    return paging_user_range_ok(uptr, len);
}

int copy_from_user(void *kdst, u32 usrc, u32 len)
{
    u8 *dst = (u8 *)kdst;
    while (len) {
        u32 page_left = 0x1000u - (usrc & 0xFFFu);
        u32 n = len < page_left ? len : page_left;

        if (!user_ok(usrc, n))
            return -1;
        memcpy(dst, (const void *)usrc, n);
        dst += n;
        usrc += n;
        len -= n;
    }
    return 0;
}

int copy_to_user(u32 udst, const void *ksrc, u32 len)
{
    const u8 *src = (const u8 *)ksrc;
    while (len) {
        u32 page_left = 0x1000u - (udst & 0xFFFu);
        u32 n = len < page_left ? len : page_left;

        if (!user_ok(udst, n))
            return -1;
        memcpy((void *)udst, src, n);
        src += n;
        udst += n;
        len -= n;
    }
    return 0;
}

int user_strncpy(char *kdst, u32 usrc, u32 max)
{
    u32 i;

    if (!max)
        return -1;
    for (i = 0; i < max - 1; i++) {
        if (!user_ok(usrc + i, 1))
            return -1;
        kdst[i] = *(const char *)(usrc + i);
        if (!kdst[i])
            return 0;
    }
    kdst[max - 1] = 0;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  有界字符串
 * ------------------------------------------------------------------ */
u32 kstrlcpy(char *dst, const char *src, u32 size)
{
    u32 i = 0;

    if (!size)
        return strlen(src);
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i + strlen(src + i);
}

u32 kstrlcat(char *dst, const char *src, u32 size)
{
    u32 d = 0, i = 0;

    while (dst[d] && d < size)
        d++;
    if (d >= size)
        return size + strlen(src);
    while (src[i] && d + i + 1 < size) {
        dst[d + i] = src[i];
        i++;
    }
    dst[d + i] = 0;
    return d + i + strlen(src + i);
}

/* ------------------------------------------------------------------ *
 *  临界区
 * ------------------------------------------------------------------ */
u32 irq_save(void)
{
    u32 flags;

    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void irq_restore(u32 flags)
{
    if (flags & 0x200)
        __asm__ volatile("sti" ::: "memory");
    else
        __asm__ volatile("cli" ::: "memory");
}
