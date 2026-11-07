#ifndef APTOS_HEAP_H
#define APTOS_HEAP_H

#include "types.h"

void  heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kmalloc_a(size_t size, size_t align);
u32   heap_used(void);
u32   heap_base(void);
u32   heap_total(void);
u32   mem_total_kb(void);
u32   mem_top_addr(void);
u32   mem_free_kb(void);

#endif
