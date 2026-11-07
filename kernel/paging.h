#ifndef APTOS_PAGING_H
#define APTOS_PAGING_H

#include "types.h"

#define PAGE_SIZE 4096

#define USER_BASE  0xC0000000u
#define USER_LIMIT 0xF0000000u

void paging_init(void);
int  paging_map_user(u32 vaddr, u32 paddr, u32 size, int writable);
int  paging_map_user_io(u32 vaddr, u32 paddr, u32 size);
int  paging_map_user_pd(u32 pd_phys, u32 vaddr, u32 paddr, u32 size, int writable);
u32  paging_clone_user_pd(void);
void paging_switch_pd(u32 pd_phys);
u32  paging_current_pd(void);
int  paging_unmap_user(u32 vaddr, u32 size);
int  paging_user_range_ok(u32 vaddr, u32 len);
int  paging_map_kernel(u32 phys, u32 size);
int  paging_map_nocache(u32 phys, u32 size);
u32  paging_user_base(void);
u32  paging_user_limit(void);
u32  paging_directory(void);
void paging_dump(void);

#endif
