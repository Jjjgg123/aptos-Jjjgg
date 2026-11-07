/* devman —— 设备管理器：列出系统检测到的硬件设备 */
#include "aptos.h"

int main(void)
{
    sysinfo_t *si = (sysinfo_t *)malloc(sizeof(sysinfo_t));
    int rr = sys_sysinfo(si);
    printf("devman: ret=%d size=%d\n", rr, (int)sizeof(*si));
    if (!si || rr < (int)sizeof(*si) || si->magic != 0x49505341u) {
        printf("devman: sysinfo unavailable\n");
        return 1;
    }

    int w = 560, h = 460;
    if (sys_win_open("Device Manager", w, h) != 0)
        return 1;

    sys_clear(RGB(20, 26, 36));
    sys_text(12, 8, "Device Manager", RGB(180, 220, 255), 2);

    /* CPU */
    sys_text(12, 38, "CPU:", RGB(150, 180, 210), 1);
    char line[160];
    snprintf(line, sizeof(line), "  %s %s", si->cpu_vendor, si->cpu_model);
    sys_text(70, 38, line, RGB(210, 220, 230), 1);

    /* 内存 */
    snprintf(line, sizeof(line), "Memory: %u MiB total, %u MiB free",
             si->mem_total_kb / 1024, si->mem_free_kb / 1024);
    sys_text(12, 60, line, RGB(150, 180, 210), 1);

    /* 设备列表 */
    sys_text(12, 88, "Devices", RGB(180, 220, 255), 2);
    int y = 114;
    for (unsigned i = 0; i < si->dev_count && i < 8; i++) {
        sys_text(12, y, si->devs[i], RGB(190, 200, 215), 1);
        y += 20;
    }

    /* 挂载 */
    y += 8;
    sys_text(12, y, "Mounted volumes", RGB(180, 220, 255), 2);
    y += 26;
    for (unsigned i = 0; i < si->mount_count && i < 8; i++) {
        snprintf(line, sizeof(line), "  %s on %s  %u/%u MiB free",
                 si->mounts[i].dev, si->mounts[i].point,
                 si->mounts[i].free_kb / 1024, si->mounts[i].total_kb / 1024);
        sys_text(12, y, line, RGB(190, 200, 215), 1);
        y += 20;
    }

    sys_text(12, h - 28, "click or wait 8s to close", RGB(120, 140, 160), 1);
    sys_flush();

    /* 等点击或 8 秒超时 */
    for (int t = 0; t < 8 * 20; t++) {
        int mx, my, mb, inside;
        sys_mouse(&mx, &my, &mb, &inside);
        if (inside && mb != 0)
            break;
        sys_sleep_ms(50);
    }
    free(si);
    return 0;
}
