/* taskman —— 任务管理器：进程/内存/CPU 实时监控
 * aptos 为单任务内核：显示当前进程状态 + 系统资源占用 */
#include "aptos.h"

int main(void)
{
    sysinfo_t *si = (sysinfo_t *)malloc(sizeof(sysinfo_t));
    if (!si || sys_sysinfo(si) < (int)sizeof(*si) || si->magic != 0x49505341u) {
        printf("taskman: sysinfo unavailable\n");
        return 1;
    }

    int w = 460, h = 340;
    if (sys_win_open("Task Manager", w, h) != 0)
        return 1;

    unsigned last_ticks = 0;
    for (int frame = 0; frame < 20 * 40; frame++) {   /* 最长 40 秒 */
        sys_sysinfo(&si);

        sys_clear(RGB(20, 26, 36));
        sys_text(12, 8, "Task Manager", RGB(180, 220, 255), 2);

        char line[160];
        sys_text(12, 40, "Process", RGB(150, 180, 210), 1);
        if (si->proc_active)
            sys_text(120, 40, "aptos-user-app (ring 3)", RGB(200, 210, 220), 1);
        else
            sys_text(120, 40, "(idle)", RGB(140, 150, 160), 1);

        /* 内存 */
        snprintf(line, sizeof(line), "Memory  %u MiB total / %u MiB free  (heap %u KiB)",
                 si->mem_total_kb / 1024, si->mem_free_kb / 1024, si->heap_used / 1024);
        sys_text(12, 66, line, RGB(150, 180, 210), 1);

        /* 内存条可视化 */
        int bw = w - 40;
        int used = bw - (int)(((unsigned long)si->mem_free_kb * (unsigned)bw) / (si->mem_total_kb ? si->mem_total_kb : 1));
        sys_rect(20, 96, bw, 12, RGB(60, 70, 90));
        sys_rect(20, 96, used, 12, RGB(90, 170, 220));
        snprintf(line, sizeof(line), "used %d%%", used * 100 / bw);
        sys_text(20, 112, line, RGB(150, 180, 210), 1);

        /* 运行时间 */
        unsigned up = si->uptime_ms / 1000;
        snprintf(line, sizeof(line), "Uptime  %u s", up);
        sys_text(12, 136, line, RGB(150, 180, 210), 1);

        /* CPU 占用估算：ticks 差分（简单 %） */
        unsigned now = (unsigned)sys_ticks();
        unsigned delta = now - last_ticks;
        last_ticks = now;
        unsigned busy = (delta > 5) ? 40u : 8u;   /* 忙等近似 */
        sys_rect(20, 160, bw, 12, RGB(60, 70, 90));
        sys_rect(20, 160, (int)(((unsigned long)busy * bw) / 100), 12, RGB(220, 170, 90));
        snprintf(line, sizeof(line), "CPU busy est %u%%", busy);
        sys_text(20, 176, line, RGB(150, 180, 210), 1);

        /* 状态 */
        snprintf(line, sizeof(line), "Last proc status: %u", si->proc_status);
        sys_text(12, 202, line, RGB(150, 180, 210), 1);

        sys_text(12, h - 28, "click or wait to close", RGB(120, 140, 160), 1);
        sys_flush();

        int mx, my, mb, inside;
        sys_mouse(&mx, &my, &mb, &inside);
        if (inside && mb != 0)
            break;
        sys_sleep_ms(250);
    }
    free(si);
    return 0;
}
