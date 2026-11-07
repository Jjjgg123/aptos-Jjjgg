#include "heap.h"
#include "bootinfo.h"
#include "klog.h"
#include "util.h"

extern u8 _kernel_end[];

#define MEM_TOP_CAP 0xC0000000u

static u32 heap_start;
static u32 heap_ptr;
static u32 heap_limit;
static u32 total_kb;
static u32 mem_top;

void heap_init(void)
{
    u32 start = ((u32)_kernel_end + 15u) & ~15u;

    if (start < 0x100000u)
        start = 0x100000u;

    heap_start = start;
    heap_ptr = start;

    total_kb = 0;
    mem_top = 0x100000u;

    e820_entry_t *map = (e820_entry_t *)BOOTINFO->e820_addr;
    for (u32 i = 0; i < BOOTINFO->e820_count; i++) {
        if (map[i].type == 1 && map[i].length > 0) {
            u64 end = map[i].base + map[i].length;
            if (end > (u64)MEM_TOP_CAP)
                end = (u64)MEM_TOP_CAP;
            if (end > 0x100000ULL) {
                total_kb += (u32)((end - 0x100000ULL) / 1024ULL);
                if ((u32)end > mem_top)
                    mem_top = (u32)end;
            }
        }
    }

    if (BOOTINFO->e820_count == 0) {
        /* 老 BIOS / 某些虚拟机取不到 E820：保守假设 16 MiB，别退化成 2 MiB */
        mem_top = 0x100000u + 16u * 1024u * 1024u;
        kprintf("[mem] WARNING: no usable E820 entries, assuming 16 MiB\n");
    } else if (mem_top <= 0x200000u) {
        mem_top = 0x200000u;
        kprintf("[mem] WARNING: memory map reports < 2 MiB usable\n");
    }
    mem_top &= ~0xFFFu;

    heap_limit = mem_top - (1u << 20);
    if (heap_limit <= heap_start)
        heap_limit = heap_start + (1u << 20);
}

u32 mem_top_addr(void)
{
    return mem_top;
}

void *kmalloc_a(size_t size, size_t align)
{
    if (align < 1)
        align = 1;
    u32 p = (heap_ptr + (u32)align - 1u) & ~((u32)align - 1u);
    if (p + size > heap_limit) {
        kprintf("[heap] out of memory (%u bytes requested)\n", (u32)size);
        return NULL;
    }
    heap_ptr = p + (u32)size;
    return (void *)p;
}

void *kmalloc(size_t size)
{
    return kmalloc_a(size, 8);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

u32 heap_used(void)
{
    return heap_ptr - heap_start;
}

u32 heap_base(void)
{
    return heap_start;
}

u32 heap_total(void)
{
    return heap_limit - heap_start;
}

u32 mem_total_kb(void)
{
    return total_kb;
}

u32 mem_free_kb(void)
{
    return mem_total_kb() - (heap_used() / 1024);
}
