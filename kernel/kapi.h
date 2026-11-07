#ifndef APTOS_KAPI_H
#define APTOS_KAPI_H

#include "types.h"

/* ---- 内核堆：可释放、带块头校验、按需向 bump 堆要页 ---- */
void  kheap_init(void);
void *kmem_alloc(size_t size);
void *kmem_calloc(size_t n, size_t size);
void *kmem_realloc(void *p, size_t size);
void  kmem_free(void *p);
void  kheap_stats(u32 *arena_kb, u32 *used_kb, u32 *blocks);
void  kheap_selftest(void);

/* ---- 致命错误：打印原因并停机（不返回） ---- */
void  kpanic(const char *fmt, ...) __attribute__((noreturn));

#define KASSERT(cond, msg)                                                   \
    do {                                                                     \
        if (!(cond))                                                         \
            kpanic("assert failed: %s (%s:%d)", msg, __FILE__, __LINE__);    \
    } while (0)

/* ---- 用户指针安全访问（稳定性：内核绝不直接解引用用户地址） ---- */
int   user_ok(u32 uptr, u32 len);
int   copy_from_user(void *kdst, u32 usrc, u32 len);
int   copy_to_user(u32 udst, const void *ksrc, u32 len);
int   user_strncpy(char *kdst, u32 usrc, u32 max);

/* ---- 有界字符串（不会越界，返回实际长度） ---- */
u32   kstrlcpy(char *dst, const char *src, u32 size);
u32   kstrlcat(char *dst, const char *src, u32 size);

/* ---- 单核临界区：返回中断标志，配 irq_restore 使用 ---- */
u32   irq_save(void);
void  irq_restore(u32 flags);

#endif
