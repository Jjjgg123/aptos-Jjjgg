#ifndef APTOS_IO_H
#define APTOS_IO_H

#include "types.h"

static inline void outb(u16 port, u8 val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(u16 port, u16 val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u16 inw(u16 port)
{
    u16 ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(u32 port, u32 val)
{
    __asm__ volatile("outl %%eax, %%dx" : : "a"(val), "d"(port));
}

static inline u32 inl(u32 port)
{
    u32 ret;
    __asm__ volatile("inl %%dx, %%eax" : "=a"(ret) : "d"(port));
    return ret;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline void insw(u16 port, void *buf, u32 count)
{
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(u16 port, const void *buf, u32 count)
{
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

static inline void rep_movsl(void *dst, const void *src, u32 dwords)
{
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(dwords) : : "memory");
}

static inline u32 irq_save(void)
{
    u32 flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(u32 flags)
{
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory");
}

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

#endif
