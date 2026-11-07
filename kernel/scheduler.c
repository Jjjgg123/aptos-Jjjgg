#include "scheduler.h"
#include "elf.h"
#include "gdt.h"
#include "io.h"
#include "timer.h"
#include "heap.h"
#include "klog.h"
#include "paging.h"
#include "user.h"
#include "util.h"

int smp_cpu_count = 1;      /* ACPI MADT 识别（单核 Bochs=1，真机多核>1） */
int smp_cpu_id(void)       /* 当前核：单核恒 0；多核时读 LAPIC ID */
{
    return 0;
}
extern int acpi_cpu_count(void);

static task_t tasks[MAX_TASKS];
static int    cur = -1;
static u32    idle_ksp;
static int    need_switch;   /* syscall 阻塞/退出后由 isr 尾部执行切换 */
static u32    sched_kernel_esp;    /* 内核态被 PIT 打断时挂起的中断帧 */
static int    sched_kernel_saved;

/* ---- 内核 idle：无用户任务可跑时 hlt 等待 PIT，由中断里切走 ---- */
static void sched_idle_loop(void)
{
    for (;;)
        hlt();
}

static void idle_frame_init(void)
{
    static u32 frame[32];
    u32 *p = frame + 32;
    *--p = 0x202;                    /* eflags: IF */
    *--p = 0x08;                     /* cs = SEL_KCODE（CPL0 iret 不弹 ss/esp） */
    *--p = (u32)sched_idle_loop;     /* eip */
    *--p = 0;                        /* int_no（isr_common add esp,8 丢弃） */
    *--p = 0;                        /* err */
    p -= 8; p[0]=0;p[1]=0;p[2]=0;p[3]=0;p[4]=0;p[5]=0;p[6]=0;p[7]=0; /* pusha */
    p -= 4; p[0]=0x10;p[1]=0x10;p[2]=0x10;p[3]=0x10;                  /* gs fs es ds */
    idle_ksp = (u32)p;
}

void scheduler_init(void)
{
    memset(tasks, 0, sizeof(tasks));
    for (int i = 0; i < MAX_TASKS; i++)
        tasks[i].id = i;
    cur = -1;
    need_switch = 0;
    idle_frame_init();
}

static task_t *task_at(int i)
{
    return (i >= 0 && i < MAX_TASKS) ? &tasks[i] : NULL;
}

static int find_free(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_FREE)
            return i;
    return -1;
}

static void wake_expired(u32 ticks)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_BLOCKED && ticks >= tasks[i].wake_tick)
            tasks[i].state = TASK_READY;
    }
}

static int pick_next(void)
{
    int best = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        int t = (cur + i) % MAX_TASKS;
        if (t < 0)
            t += MAX_TASKS;
        if (t != cur &&
            (tasks[t].state == TASK_READY || tasks[t].state == TASK_RUNNING)) {
            if (best < 0 || tasks[t].prio > tasks[best].prio)
                best = t;
        }
    }
    return best;
}

static void switch_to(int next)
{
    if (next < 0) {
        /* 无任务可跑：切到内核 idle */
        cur = -1;
        tss_set_stack(0x90000);
        return;
    }
    if (cur >= 0 && tasks[cur].state == TASK_RUNNING)
        tasks[cur].state = TASK_READY;
    kprintf("[sched] switch -> task %d '%s'\n", next, tasks[next].name);
    cur = next;
    tasks[next].state = TASK_RUNNING;
    tasks[next].quantum = QUANTUM_TICKS + (u32)(tasks[next].prio * 2);
    tss_set_stack((u32)tasks[next].kstack + KSTACK_SIZE - 16);
    if (tasks[next].is_user && tasks[next].pd != paging_current_pd())
        paging_switch_pd(tasks[next].pd);
}

static int pick_first(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING)
            return i;
    }
    return -1;
}

u32 scheduler_poll(regs_t *r)
{
    u32 ticks = timer_ticks();

    wake_expired(ticks);

    /* 无当前任务但有就绪任务：由首次 PIT 中断切入 */
    if (cur < 0) {
        int f = pick_first();
        if (f < 0) {
            if (sched_kernel_saved) {
                u32 esp = sched_kernel_esp;
                sched_kernel_saved = 0;
                return esp;
            }
            return 0;
        }
        sched_kernel_esp = (u32)r;
        sched_kernel_saved = 1;
        switch_to(f);
        return tasks[f].ksp;
    }

    if (need_switch && cur >= 0 && tasks[cur].is_user) {
        /* syscall 里请求了切换（睡眠/退出/让出） */
        tasks[cur].ksp = (u32)r;
        need_switch = 0;
        int next = pick_next();
        if (next < 0) {
            /* 没有别的就绪任务。注意：pick_next 永远不返回 cur 自己，所以这里
               "无就绪任务"包括"全系统只有当前这一个用户任务在跑"。
               如果此时让它原地续跑，内核上下文（gui_run → gui_poll）就再也
               回不来了 —— GUI 被单个长时间运行的程序整个冻住，表现就是
               GL3D 一启动鼠标和刷新全停。这里把内核还回去跑一轮，
               内核下次 hlt 后的 PIT 中断会把用户任务再接回来。 */
            if ((tasks[cur].state == TASK_RUNNING || tasks[cur].state == TASK_READY)
                && sched_kernel_saved) {
                u32 esp = sched_kernel_esp;
                tasks[cur].state = TASK_RUNNING;
                tasks[cur].quantum = QUANTUM_TICKS;
                sched_kernel_saved = 0;
                cur = -1;
                return esp;
            }
            if (tasks[cur].state == TASK_RUNNING || tasks[cur].state == TASK_READY) {
                tasks[cur].state = TASK_RUNNING;
                return 0;   /* 当前任务继续（没有内核上下文可回） */
            }
            if (sched_kernel_saved) {
                u32 esp = sched_kernel_esp;
                cur = -1;
                sched_kernel_saved = 0;
                return esp;
            }
            switch_to(-1);
            return idle_ksp;
        }
        switch_to(next);
        return tasks[next].ksp;
    }

    if (cur >= 0 && tasks[cur].is_user) {
        if (tasks[cur].state == TASK_RUNNING && tasks[cur].quantum == 0) {
            /* 时间片耗尽：抢占切换（无其他就绪任务则不切） */
            tasks[cur].ksp = (u32)r;
            int next = pick_next();
            if (next < 0 || next == cur) {
                tasks[cur].state = TASK_RUNNING;
                tasks[cur].quantum = QUANTUM_TICKS;
                return 0;
            }
            switch_to(next);
            return tasks[next].ksp;
        }
    }
    return 0;
}

void scheduler_tick(void)
{
    if (cur >= 0 && tasks[cur].state == TASK_RUNNING && tasks[cur].quantum > 0)
        tasks[cur].quantum--;
}

void scheduler_yield(void)
{
    if (cur >= 0 && tasks[cur].is_user)
        need_switch = 1;
}

void scheduler_block_ms(u32 ms)
{
    if (cur < 0 || !tasks[cur].is_user) {
        timer_sleep_ms(ms);
        return;
    }
    if (scheduler_count() > 1) {
        tasks[cur].state = TASK_BLOCKED;
        tasks[cur].wake_tick = timer_ticks() + (ms * timer_hz()) / 1000u + 1u;
        need_switch = 1;
    } else {
        /* 唯一任务：降级忙等（保持简单可靠） */
        timer_sleep_ms(ms);
    }
}

void scheduler_exit(int status)
{
    if (cur < 0)
        return;
    tasks[cur].status = status;
    tasks[cur].state = TASK_ZOMBIE;
    need_switch = 1;
}

/* ---- 用户任务帧构造：iret 进用户态 ---- */
static void build_user_frame(task_t *t, u32 entry)
{
    u32 *st = (u32 *)((u8 *)t->kstack + KSTACK_SIZE);
    *--st = 0x23;                  /* ss */
    *--st = USER_STACK_TOP - 16;   /* 用户 esp */
    *--st = 0x202;                 /* eflags: IF */
    *--st = 0x1B;                  /* cs = SEL_UCODE */
    *--st = entry;                 /* eip */
    *--st = 0;                     /* int_no（isr_common add esp,8 丢弃） */
    *--st = 0;                     /* err */
    *--st = 0; *--st = 0; *--st = 0; *--st = USER_STACK_TOP - 16;  /* edi esi ebp esp */
    *--st = 0; *--st = 0; *--st = 0; *--st = 0;                    /* ebx edx ecx eax */
    *--st = 0x23; *--st = 0x23; *--st = 0x23; *--st = 0x23;        /* gs fs es ds */
    t->ksp = (u32)st;
}

int scheduler_spawn(const void *elf, u32 size, const char *name,
                    const char *arg0, const char *arg1)
{
    int i = find_free();
    task_t *t;
    elf_image_t img;
    u32 stack_phys, saved_pd;
    int r;

    if (i < 0)
        return -1;
    {
        static int smp_printed = 0;
        if (!smp_printed) {
            smp_cpu_count = acpi_cpu_count() > 0 ? acpi_cpu_count() : 1;
            kprintf("[sched] smp: %d cpu(s), this cpu %d\n", smp_cpu_count, smp_cpu_id());
            smp_printed = 1;
        }
    }

    /* 整个 spawn 过程禁中断：任务在帧构造完成前不可被调度切入 */
    __asm__ volatile("cli");

    t = &tasks[i];
    memset(t, 0, sizeof(*t));
    t->id = i;
    t->state = TASK_FREE;   /* 完成初始化后才置 READY */
    t->is_user = 1;
    t->quantum = QUANTUM_TICKS;
    t->prio = 1;
    if (name)
        strncpy(t->name, name, sizeof(t->name) - 1);
    if (arg0)
        strncpy(t->arg0, arg0, sizeof(t->arg0) - 1);
    if (arg1)
        strncpy(t->arg1, arg1, sizeof(t->arg1) - 1);

    t->kstack = (u8 *)kmalloc(KSTACK_SIZE);
    if (!t->kstack) {
        tasks[i].state = TASK_FREE;
        return -3;
    }

    t->pd = paging_clone_user_pd();
    if (!t->pd) {
        tasks[i].state = TASK_FREE;
        __asm__ volatile("sti");
        return -4;
    }

    /* 用户栈（独立物理页，映射到本任务页目录的用户窗口） */
    stack_phys = (u32)kmalloc_a(USER_STACK_SIZE, PAGE_SIZE);
    if (!stack_phys) {
        tasks[i].state = TASK_FREE;
        __asm__ volatile("sti");
        return -5;
    }
    memset((void *)stack_phys, 0, USER_STACK_SIZE);
    t->user_stack = stack_phys;
    if (paging_map_user_pd(t->pd, USER_STACK_TOP - USER_STACK_SIZE, stack_phys,
                           USER_STACK_SIZE, 1) != 0) {
        tasks[i].state = TASK_FREE;
        __asm__ volatile("sti");
        return -6;
    }

    /* ELF 段直接加载到本任务页目录（切换全局 pd，elf_load 的
       paging_map_user 即映射到本任务） */
    saved_pd = paging_current_pd();
    paging_switch_pd(t->pd);
    r = elf_load(elf, size, &img);
    paging_switch_pd(saved_pd);
    if (r != 0) {
        tasks[i].state = TASK_FREE;
        __asm__ volatile("sti");
        return -7;
    }

    t->brk_base = (img.base + img.size + 0xFFFu) & ~0xFFFu;
    t->brk_top = t->brk_base;

    build_user_frame(t, img.entry);
    t->state = TASK_READY;
    __asm__ volatile("sti");
    kprintf("[sched] task %d '%s' spawned eip=0x%x pd=0x%x\n",
            i, t->name, img.entry, t->pd);
    return i;
}

void scheduler_reap_zombie(int id)
{
    task_t *t = task_at(id);
    if (!t || t->state != TASK_ZOMBIE)
        return;
    t->state = TASK_FREE;
    t->name[0] = 0;
    t->arg0[0] = 0;
    t->arg1[0] = 0;
}

int scheduler_kill(int id)
{
    task_t *t = task_at(id);
    if (!t || t->state == TASK_FREE)
        return -1;
    if (id == cur) {
        scheduler_exit(-1);
        return 0;
    }
    t->state = TASK_ZOMBIE;
    t->status = -1;
    return 0;
}

int scheduler_alive(int id)
{
    task_t *t = task_at(id);
    return t && (t->state == TASK_READY || t->state == TASK_RUNNING ||
                 t->state == TASK_BLOCKED);
}

int scheduler_status(int id)
{
    task_t *t = task_at(id);
    return t ? t->status : -1;
}

int scheduler_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_FREE)
            n++;
    return n;
}

int scheduler_cur_id(void)
{
    return cur;
}

int scheduler_cur_user(void)
{
    return cur >= 0 && tasks[cur].is_user;
}

const char *scheduler_name(int id)
{
    task_t *t = task_at(id);
    return t ? t->name : "";
}

u32 scheduler_brk(void)
{
    return cur >= 0 ? tasks[cur].brk_top : 0;
}

u32 scheduler_brk_base(void)
{
    return cur >= 0 ? tasks[cur].brk_base : 0;
}

void scheduler_set_brk_base(u32 top)
{
    if (cur >= 0)
        tasks[cur].brk_base = top;
}

void scheduler_set_brk(u32 top)
{
    if (cur >= 0)
        tasks[cur].brk_top = top;
}

void scheduler_get_args(char *a0, char *a1, int max)
{
    if (cur < 0)
        return;
    strncpy(a0, tasks[cur].arg0, max - 1);
    a0[max - 1] = 0;
    if (a1) {
        strncpy(a1, tasks[cur].arg1, max - 1);
        a1[max - 1] = 0;
    }
}
