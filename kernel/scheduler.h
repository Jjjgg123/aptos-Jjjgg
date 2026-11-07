#ifndef APTOS_SCHED_H
#define APTOS_SCHED_H

#include "types.h"
#include "idt.h"

#define MAX_TASKS     8
#define KSTACK_SIZE   (8 * 1024)
#define QUANTUM_TICKS 5           /* 时间片：100Hz 下 50ms（更灵敏抢占，防单任务卡屏） */

typedef enum {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,   /* 睡眠中 */
    TASK_ZOMBIE     /* 已退出，等待回收 */
} task_state_t;

typedef struct {
    int          id;
    task_state_t state;
    int          is_user;
    u32          pd;              /* 独立页目录（用户进程） */
    u8          *kstack;          /* 内核栈基址 */
    u32          ksp;             /* 保存的内核栈指针（切换点） */
    u32          user_stack;      /* 用户栈物理页 */
    u32          brk_base;        /* 用户堆起点 */
    u32          brk_top;         /* 用户堆顶 */
    char         arg0[160];
    char         arg1[160];
    char         name[24];
    int          status;          /* 退出码 */
    int          prio;            /* 调度优先级（越大越优先，默认 1） */
    u32          quantum;         /* 剩余时间片 */
    u32          wake_tick;       /* 阻塞唤醒时刻 */
} task_t;

void scheduler_init(void);
int  scheduler_spawn(const void *elf, u32 size, const char *name,
                     const char *arg0, const char *arg1);
u32  scheduler_poll(regs_t *r);   /* 中断尾部：需要则切换，返回新 esp 或 0 */
void scheduler_tick(void);        /* PIT 每 tick：扣当前任务时间片 */
void scheduler_block_ms(u32 ms);  /* 当前任务睡眠（syscall 中调用） */
void scheduler_exit(int status);  /* 当前任务退出 */
void scheduler_yield(void);       /* 主动让出（配合 poll 切换） */
int  scheduler_kill(int id);      /* 强杀 */
int  scheduler_alive(int id);
int  scheduler_status(int id);
int  scheduler_count(void);       /* 非 FREE 任务数 */
int  scheduler_cur_id(void);
int  scheduler_cur_user(void);    /* 当前任务是否用户态 */
const char *scheduler_name(int id);
u32  scheduler_brk(void);
u32  scheduler_brk_base(void);
void scheduler_set_brk_base(u32 top);
void scheduler_set_brk(u32 top);
void scheduler_get_args(char *a0, char *a1, int max);
void scheduler_reap_zombie(int id);

#endif
