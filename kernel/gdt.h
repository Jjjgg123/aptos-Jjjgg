#ifndef APTOS_GDT_H
#define APTOS_GDT_H

#include "types.h"

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x1B
#define SEL_UDATA 0x23
#define SEL_TSS   0x28

void gdt_init(void);
void tss_set_stack(u32 esp0);

#endif
