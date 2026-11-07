#include "paging.h"
#include "bootinfo.h"
#include "heap.h"
#include "klog.h"
#include "util.h"

#define PDE_PRESENT 0x01
#define PDE_RW      0x02
#define PDE_USER    0x04

static u32 *pd;
static u32 enabled;
static u32 fb_mapped_kb;

extern u8 __text_start[], __text_end[];
extern u8 __rodata_start[], __rodata_end[];
extern u8 stack_guard[];

static void flush_tlb(void)
{
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
}

static u32 *pt_for(u32 vaddr, int user, int create)
{
    u32 idx = vaddr >> 22;
    u32 entry = pd[idx];

    if (entry & PDE_PRESENT)
        return (u32 *)(entry & ~0xFFFu);
    if (!create)
        return NULL;
    {
        u32 *pt = (u32 *)kmalloc_a(PAGE_SIZE, PAGE_SIZE);
        if (!pt)
            return NULL;
        memset(pt, 0, PAGE_SIZE);
        pd[idx] = (u32)pt | PDE_PRESENT | PDE_RW | (user ? PDE_USER : 0);
        return pt;
    }
}

/* 32 位非 PAE 页表没有 NX 位，W^X 的 X 无法强制；
 * 但可以把代码段/只读常量所在页的 RW 位清掉，做到“不可写”。 */
static void set_readonly(u32 start, u32 end)
{
    u32 a;

    for (a = start & ~0xFFFu; a < end; a += PAGE_SIZE) {
        u32 *pt = pt_for(a, 0, 0);
        if (pt)
            pt[(a >> 12) & 0x3FF] &= ~PDE_RW;
    }
}

/* 栈底下方一页置为 not-present：栈溢出立刻 #PF，而不是静默踩 bss 静态数据 */
static void guard_stack(void)
{
    u32 g = (u32)stack_guard;
    u32 *pt = pt_for(g, 0, 0);

    if (pt) {
        pt[(g >> 12) & 0x3FF] = 0;
        flush_tlb();
    }
}

static int map_identity(u32 start, u32 size)
{
    u32 a;

    start &= ~0xFFFu;
    size = (size + 0xFFFu) & ~0xFFFu;

    for (a = start; a < start + size; a += PAGE_SIZE) {
        u32 *pt = pt_for(a, 0, 1);
        if (!pt)
            return -1;
        pt[(a >> 12) & 0x3FF] = a | PDE_PRESENT | PDE_RW;
    }
    return 0;
}

int paging_map_nocache(u32 start, u32 size)
{
    u32 a;

    start &= ~0xFFFu;
    size = (size + 0xFFFu) & ~0xFFFu;

    for (a = start; a < start + size; a += PAGE_SIZE) {
        u32 *pt = pt_for(a, 0, 1);
        if (!pt)
            return -1;
        pt[(a >> 12) & 0x3FF] = a | PDE_PRESENT | PDE_RW | 0x18;
    }
    return 0;
}

static void map_framebuffer(void)
{
    u32 fb = BOOTINFO->fb_addr;
    u32 bytes;

    fb_mapped_kb = 0;
    if (!fb)
        return;

    bytes = BOOTINFO->fb_pitch * BOOTINFO->fb_height;
    if (bytes < 1024 * 1024)
        bytes = 1024 * 1024;
    bytes += 1024 * 1024;

    /* 显存映射到 0xF0000000 内核专用高虚拟（避开用户窗 0xC0000000-0xF0000000） */
    bytes = (bytes + 0xFFFu) & ~0xFFFu;
    {
        u32 a;
        for (a = 0; a < bytes; a += PAGE_SIZE) {
            u32 *pt = pt_for(0xF0000000u + a, 0, 1);
            if (!pt) { fb_mapped_kb = 0; return; }
            pt[((0xF0000000u + a) >> 12) & 0x3FF] = (fb + a) | PDE_PRESENT | PDE_RW;
        }
    }
    fb_mapped_kb = bytes / 1024;
}

void paging_init(void)
{
    u32 top = mem_top_addr();
    u32 map_mb = (top + 0xFFFFFu) / 0x100000u;
    u32 addr;

    if (map_mb < 16)
        map_mb = 16;
    if (map_mb > 3072)
        map_mb = 3072;
    map_mb = (map_mb + 3) & ~3u;

    pd = (u32 *)kmalloc_a(PAGE_SIZE, PAGE_SIZE);
    if (!pd) {
        kprintf("[mmu] cannot allocate a page directory - paging left off\n");
        return;
    }
    memset(pd, 0, PAGE_SIZE);

    for (addr = 0; addr < map_mb * 0x100000u; addr += 0x400000u) {
        u32 *pt;

        if (addr >= USER_BASE && addr < USER_LIMIT) {
            kprintf("[mmu] WARNING: RAM reaches the user window at 0x%x\n", addr);
            break;
        }
        pt = (u32 *)kmalloc_a(PAGE_SIZE, PAGE_SIZE);
        if (!pt) {
            kprintf("[mmu] out of memory building page tables\n");
            return;
        }
        memset(pt, 0, PAGE_SIZE);
        pd[addr >> 22] = (u32)pt | PDE_PRESENT | PDE_RW;
        for (u32 i = 0; i < 1024; i++)
            pt[i] = (addr + i * PAGE_SIZE) | PDE_PRESENT | PDE_RW;
    }

    map_framebuffer();

    __asm__ volatile("mov %%eax, %%cr3" ::"a"(pd));

    {
        u32 cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= 0x80000000u;
        __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
    }

    enabled = 1;
    flush_tlb();

    /* 代码段 + 只读常量：清 PTE.RW（32 位无 NX，无法禁执行，只能禁写） */
    set_readonly((u32)__text_start, (u32)__rodata_end);
    flush_tlb();
    /* 栈下 guard 页：取消映射，栈溢出立即触发 #PF */
    guard_stack();

    kprintf("[mmu] paging on: 4 KiB pages, identity 0..%d MiB, framebuffer 0x%x (%d KiB)\n",
            map_mb, BOOTINFO->fb_addr, fb_mapped_kb);
    kprintf("[mmu] W^X: .text 0x%x..0x%x + .rodata 0x%x..0x%x read-only, stack guard at 0x%x\n",
            (u32)__text_start, (u32)__text_end, (u32)__rodata_start,
            (u32)__rodata_end, (u32)stack_guard);
}

int paging_map_user(u32 vaddr, u32 paddr, u32 size, int writable)
{
    u32 off = 0;
    (void)writable;

    if (!enabled)
        return -1;
    if (vaddr & 0xFFF)
        return -1;
    if (vaddr < paging_user_base() || vaddr + size > paging_user_limit())
        return -1;

    while (off < size) {
        u32 *pt = pt_for(vaddr + off, 1, 1);
        if (!pt)
            return -1;
        pt[((vaddr + off) >> 12) & 0x3FF] =
            (paddr + off) | PDE_PRESENT | PDE_RW | PDE_USER;
        off += PAGE_SIZE;
    }
    flush_tlb();
    return 0;
}

/* MMIO 映射：和 paging_map_user 一样，但页表项打上 PCD（cache disable）。
 * 设备寄存器必须绕过 CPU cache，否则读到的永远是第一次缓存的旧值。 */
int paging_map_user_io(u32 vaddr, u32 paddr, u32 size)
{
    u32 off = 0;

    if (!enabled)
        return -1;
    if (vaddr & 0xFFF)
        return -1;
    if (vaddr < paging_user_base() || vaddr + size > paging_user_limit())
        return -1;

    while (off < size) {
        u32 *pt = pt_for(vaddr + off, 1, 1);
        if (!pt)
            return -1;
        pt[((vaddr + off) >> 12) & 0x3FF] =
            (paddr + off) | PDE_PRESENT | PDE_RW | PDE_USER | 0x10u;
        off += PAGE_SIZE;
    }
    flush_tlb();
    return 0;
}

/* ---- 多进程页目录支持：每进程独立页目录，内核映射共享 ---- */
u32 paging_clone_user_pd(void)
{
    u32 *newpd;
    int i;

    if (!enabled)
        return 0;
    newpd = (u32 *)kmalloc_a(PAGE_SIZE, PAGE_SIZE);
    if (!newpd)
        return 0;
    memcpy(newpd, pd, PAGE_SIZE);
    /* 清空用户窗口 PDE（0xC0000000-0xD0000000），各进程独立映射 */
    for (i = (int)(USER_BASE >> 22); i < (int)((USER_LIMIT + 0x3FFFFFu) >> 22); i++)
        newpd[i] = 0;
    return (u32)newpd;
}

int paging_map_user_pd(u32 pd_phys, u32 vaddr, u32 paddr, u32 size, int writable)
{
    u32 old = (u32)pd;
    int r;

    pd = (u32 *)pd_phys;
    r = paging_map_user(vaddr, paddr, size, writable);
    pd = (u32 *)old;
    return r;
}

void paging_switch_pd(u32 pd_phys)
{
    if (!pd_phys || !enabled)
        return;
    pd = (u32 *)pd_phys;
    __asm__ volatile("mov %0, %%cr3" ::"r"(pd_phys) : "memory");
}

u32 paging_current_pd(void)
{
    return (u32)pd;
}

int paging_unmap_user(u32 vaddr, u32 size)
{
    u32 off = 0;
    while (off < size) {
        u32 *pt = pt_for(vaddr + off, 1, 0);
        if (pt)
            pt[((vaddr + off) >> 12) & 0x3FF] = 0;
        off += PAGE_SIZE;
    }
    flush_tlb();
    return 0;
}


int paging_map_kernel(u32 phys, u32 size)
{
    if (!enabled)
        return -1;
    return map_identity(phys, size);
}

int paging_user_range_ok(u32 vaddr, u32 len)
{
    u32 a, last;

    if (!enabled || len == 0)
        return 0;
    if (vaddr < paging_user_base())
        return 0;
    if (vaddr + len < vaddr)
        return 0;
    if (vaddr + len > paging_user_limit())
        return 0;

    last = vaddr + len - 1;
    for (a = vaddr & ~0xFFFu; a <= last; a += PAGE_SIZE) {
        u32 *pt = pt_for(a, 1, 0);
        u32 e;

        if (!pt)
            return 0;
        e = pt[(a >> 12) & 0x3FF];
        if (!(e & 1) || !(e & PDE_USER))
            return 0;
        if (a + PAGE_SIZE < a)
            break;
    }
    return 1;
}

u32 paging_user_base(void) { return USER_BASE; }
u32 paging_user_limit(void) { return USER_LIMIT; }
u32 paging_directory(void) { return (u32)pd; }

void paging_dump(void)
{
    kprintf("[mmu] page directory at 0x%x, user window 0x%x..0x%x\n", (u32)pd,
            paging_user_base(), paging_user_limit());
}
