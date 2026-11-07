#include "gdt.h"
#include "klog.h"
#include "util.h"

typedef struct PACKED {
    u32 prev_tss;
    u32 esp0;
    u32 ss0;
    u32 esp1;
    u32 ss1;
    u32 esp2;
    u32 ss2;
    u32 cr3;
    u32 eip;
    u32 eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs;
    u32 ldt;
    u16 trap;
    u16 iomap_base;
} tss_t;

typedef struct PACKED {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  flags;
    u8  base_hi;
} gdt_entry_t;

typedef struct PACKED {
    u16 limit;
    u32 base;
} gdt_ptr_t;

static gdt_entry_t gdt[6];
static gdt_ptr_t   gp;
static tss_t       tss;

static void set_gate(int i, u32 base, u32 limit, u8 access, u8 flags)
{
    gdt[i].limit_lo = (u16)(limit & 0xFFFF);
    gdt[i].base_lo = (u16)(base & 0xFFFF);
    gdt[i].base_mid = (u8)((base >> 16) & 0xFF);
    gdt[i].access = access;
    gdt[i].flags = (u8)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[i].base_hi = (u8)((base >> 24) & 0xFF);
}

void gdt_init(void)
{
    set_gate(0, 0, 0, 0, 0);
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xCF);
    set_gate(2, 0, 0xFFFFF, 0x92, 0xCF);
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xCF);
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xCF);

    memset(&tss, 0, sizeof(tss));
    tss.ss0 = SEL_KDATA;
    tss.esp0 = 0;
    tss.iomap_base = (u16)sizeof(tss);
    set_gate(5, (u32)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gp.limit = (u16)(sizeof(gdt) - 1);
    gp.base = (u32)&gdt;

    __asm__ volatile("lgdt %0" ::"m"(gp));
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "mov $0x28, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        :
        : "eax", "memory");

    kprintf("[gdt] kernel/user segments + TSS installed (user cs=0x%x ds=0x%x)\n",
            SEL_UCODE, SEL_UDATA);
}

void tss_set_stack(u32 esp0)
{
    tss.esp0 = esp0;
}
