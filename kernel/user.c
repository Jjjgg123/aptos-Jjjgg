#include "user.h"
#include "elf.h"
#include "gdt.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "scheduler.h"
#include "gui.h"
#include "syscall.h"
#include "util.h"

extern void user_enter(u32 entry, u32 stack);
extern void user_exit_asm(int status);
extern u32 user_exit_status;

static int active;
static int last_status;
static u8 *kstack;

int user_active(void)
{
    return scheduler_cur_user();
}

int user_last_status(void)
{
    return last_status;
}

void user_exit(int status)
{
    active = 0;
    last_status = status;
    user_exit_asm(status);
    kprintf("[user] WARNING: exit returned\n");
}

int user_exec_image(const void *elf, u32 size, const char *arg0, const char *arg1)
{
    elf_image_t img;
    u32 stack_phys;
    int r;

    if (active)
        return -1;
    if (elf_load(elf, size, &img) != 0) {
        kprintf("[user] not a loadable ELF32 executable\n");
        return -2;
    }

    if (!kstack)
        kstack = (u8 *)kmalloc(32768);
    if (!kstack) {
        elf_unload(&img);
        return -3;
    }
    tss_set_stack((u32)kstack + 32768 - 16);

    stack_phys = (u32)kmalloc_a(USER_STACK_SIZE, PAGE_SIZE);
    if (!stack_phys) {
        elf_unload(&img);
        return -4;
    }
    memset((void *)stack_phys, 0, USER_STACK_SIZE);
    if (paging_map_user(USER_STACK_TOP - USER_STACK_SIZE, stack_phys,
                        USER_STACK_SIZE, 1) != 0) {
        elf_unload(&img);
        return -5;
    }

    syscall_set_args(arg0, arg1);

    syscall_brk_init((img.base + img.size + 0xFFFu) & ~0xFFFu);

    active = 1;
    last_status = 0;


    user_enter(img.entry, USER_STACK_TOP - 16);

    active = 0;
    last_status = (int)user_exit_status;
    gui_dirty();
    gui_poll();
    syscall_brk_free();
    elf_unload(&img);
    paging_unmap_user(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE);
    r = last_status;
    if (r != 0)
        kprintf("[user] process exited with status %d\n", r);
    return r;
}
