#include "idt.h"
#include "io.h"
#include "klog.h"
#include "scheduler.h"
#include "syscall.h"
#include "user.h"
#include "util.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

#define IDT_ENTRIES 256

typedef struct PACKED {
    u16 offset_low;
    u16 selector;
    u8  zero;
    u8  flags;
    u16 offset_high;
} idt_entry_t;

typedef struct PACKED {
    u16 limit;
    u32 base;
} idt_ptr_t;

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idt_ptr;
static irq_handler_t irq_handlers[16];

extern u32 isr_stub_table[];
extern void isr128(void);

static void idt_set_gate(int n, u32 handler, u16 selector, u8 flags)
{
    idt[n].offset_low  = (u16)(handler & 0xFFFF);
    idt[n].selector    = selector;
    idt[n].zero        = 0;
    idt[n].flags       = flags;
    idt[n].offset_high = (u16)((handler >> 16) & 0xFFFF);
}

static void pic_remap(void)
{
    u8 mask1 = inb(PIC1_DATA);
    u8 mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11);   io_wait();
    outb(PIC2_CMD, 0x11);   io_wait();
    outb(PIC1_DATA, 0x20);  io_wait();
    outb(PIC2_DATA, 0x28);  io_wait();
    outb(PIC1_DATA, 0x04);  io_wait();
    outb(PIC2_DATA, 0x02);  io_wait();
    outb(PIC1_DATA, 0x01);  io_wait();
    outb(PIC2_DATA, 0x01);  io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    (void)mask1;
    (void)mask2;
}

void irq_unmask(int irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit = (u8)(1u << (irq & 7));
    outb(port, (u8)(inb(port) & ~bit));
    if (irq >= 8)
        outb(PIC1_DATA, (u8)(inb(PIC1_DATA) & ~(1u << 2)));
}

void irq_mask(int irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit = (u8)(1u << (irq & 7));
    outb(port, (u8)(inb(port) | bit));
}

void irq_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void irq_register(int irq, irq_handler_t handler)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

static const char *exception_name(u32 n)
{
    static const char *names[32] = {
        "divide by zero", "debug", "non-maskable interrupt", "breakpoint",
        "overflow", "bound range exceeded", "invalid opcode",
        "device not available", "double fault", "coprocessor segment",
        "invalid TSS", "segment not present", "stack-segment fault",
        "general protection fault", "page fault", "reserved",
        "x87 floating point", "alignment check", "machine check",
        "SIMD floating point", "virtualisation", "control protection",
        "reserved", "reserved", "reserved", "reserved", "reserved",
        "reserved", "hypervisor", "VMM communication", "security",
        "reserved"
    };
    return (n < 32) ? names[n] : "unknown";
}

u32 isr_handler(regs_t *r)
{
    if (r->int_no == 0x80) {
        syscall_dispatch(r);
        return scheduler_poll(r);
    }

    if (r->int_no < 32) {
        if (user_active()) {
            u32 cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            kprintf("\n[user] exception %d (%s) at eip=0x%x, cr2=0x%x - process killed\n",
                    r->int_no, exception_name(r->int_no), r->eip, cr2);
            kprintf("[user] eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
                    r->eax, r->ebx, r->ecx, r->edx);
            kprintf("[user] esi=0x%x edi=0x%x ebp=0x%x esp=0x%x\n",
                    r->esi, r->edi, r->ebp, r->esp);
            /* dump 用户栈（ebp 起 24 字）定位调用者返回地址 */
            {
                u32 *usp = (u32 *)r->ebp;
                for (int i = 0; i < 24; i++) {
                    u32 *q = usp + i;
                    if ((u32)q < 0xc0000000u || (u32)q >= 0xef800000u) break;
                    kprintf("[user] stk%d = 0x%x\n", i, *q);
                }
            }
            scheduler_exit(-1);
            return scheduler_poll(r);
        }
        kprintf("\n[panic] exception %d: %s\n", r->int_no, exception_name(r->int_no));
        kprintf("        err=0x%x eip=0x%x cs=0x%x eflags=0x%x\n",
                r->err_code, r->eip, r->cs, r->eflags);
        kprintf("        eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
                r->eax, r->ebx, r->ecx, r->edx);
        kprintf("        system halted.\n");
        for (;;)
            hlt();
        return 0;
    }

    int irq = (int)r->int_no - 32;
    if (irq >= 0 && irq < 16 && irq_handlers[irq])
        irq_handlers[irq](r);
    irq_eoi(irq);
    return scheduler_poll(r);
}


void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    memset(irq_handlers, 0, sizeof(irq_handlers));

    idt_ptr.limit = (u16)(sizeof(idt_entry_t) * IDT_ENTRIES - 1);
    idt_ptr.base  = (u32)&idt;

    for (int i = 0; i < 48; i++)
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    idt_set_gate(0x80, (u32)isr128, 0x08, 0xEE);

    pic_remap();
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    kprintf("[idt] 48 vectors + int 0x80 syscall gate (DPL 3) installed\n");
}
