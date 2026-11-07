/* sysinfo.c —— 系统信息汇聚（SYS_SYSINFO=41 支持）
 * 供设备管理器/任务管理器/系统信息应用使用 */
#include "sysinfo.h"
#include "blkdev.h"
#include "fs.h"
#include "heap.h"
#include "timer.h"
#include "user.h"
#include "util.h"
#include "klog.h"

static char cpu_vendor[16];
static char cpu_model[48];
static u32  cpu_feat;

static void cpu_detect(void)
{
    /* CPUID 探测（i686+ 必有） */
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(u32 *)(cpu_vendor + 0) = ebx;
    *(u32 *)(cpu_vendor + 4) = edx;
    *(u32 *)(cpu_vendor + 8) = ecx;
    cpu_vendor[12] = 0;

    /* 品牌字符串（EAX=0x80000002..04） */
    char *p = cpu_model;
    for (u32 leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(leaf));
        *(u32 *)(p + 0) = eax;
        *(u32 *)(p + 4) = ebx;
        *(u32 *)(p + 8) = ecx;
        *(u32 *)(p + 12) = edx;
        p += 16;
    }
    *p = 0;

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    cpu_feat = edx;
}

void sysinfo_init(void)
{
    cpu_detect();
    kprintf("[cpu] %s %s\n", cpu_vendor, cpu_model);
}

void sysinfo_fill(sysinfo_t *si)
{
    si->magic = SYSINFO_MAGIC;
    strncpy(si->os_name, "aptos", sizeof(si->os_name) - 1);
    strncpy(si->os_ver, "2.0", sizeof(si->os_ver) - 1);
    strncpy(si->cpu_vendor, cpu_vendor, sizeof(si->cpu_vendor) - 1);
    strncpy(si->cpu_model, cpu_model, sizeof(si->cpu_model) - 1);
    si->cpu_feat = cpu_feat;
    si->mem_total_kb = mem_total_kb();
    si->mem_free_kb = mem_free_kb();
    si->heap_used = heap_used();
    si->uptime_ms = timer_uptime_ms();

    /* 设备列表：块设备 */
    si->dev_count = 0;
    int n = blk_count();
    for (int i = 0; i < n && si->dev_count < 8; i++) {
        const blk_info_t *bi = blk_info(i);
        if (!bi || !bi->present)
            continue;
        char *d = si->devs[si->dev_count];
        snprintf(d, 40, "block:%s %s %u MiB %s",
                 bi->name, blk_type_name(bi->type),
                 (u32)((u64)bi->sectors * bi->sector_size / (1024 * 1024)),
                 bi->model);
        si->dev_count++;
    }
    /* 固定外设 */
    if (si->dev_count < 8) {
        strncpy(si->devs[si->dev_count], "display:VBE linear framebuffer", 40);
        si->dev_count++;
    }
    if (si->dev_count < 8) {
        strncpy(si->devs[si->dev_count], "input:PS/2 keyboard+mouse", 40);
        si->dev_count++;
    }
    if (si->dev_count < 8) {
        strncpy(si->devs[si->dev_count], "timer:PIT 8254", 40);
        si->dev_count++;
    }
    if (si->dev_count < 8) {
        strncpy(si->devs[si->dev_count], "sound:PC Speaker", 40);
        si->dev_count++;
    }
    if (si->dev_count < 8) {
        strncpy(si->devs[si->dev_count], "rtc:CMOS clock", 40);
        si->dev_count++;
    }

    /* 挂载点 */
    si->mount_count = 0;
    fs_mountinfo_t mi[8];
    int mn = fs_mounts(mi, 8);
    for (int i = 0; i < mn && i < 8; i++) {
        si->mounts[si->mount_count++] = mi[i];
    }

    /* 进程 */
    si->proc_active = user_active();
    si->proc_status = user_last_status();
}
