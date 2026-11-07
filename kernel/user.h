#ifndef APTOS_USER_H
#define APTOS_USER_H

#include "types.h"

#define USER_STACK_TOP  0xEF800000u
#define USER_STACK_SIZE (192 * 1024) /* 192KB：让 64KB 给 brk 堆 */

int  user_exec_image(const void *elf, u32 size, const char *arg0, const char *arg1);
int  user_active(void);
void user_exit(int status);
int  user_last_status(void);
void syscall_brk_init(u32 top);
void syscall_brk_free(void);

#endif
