#ifndef APTOS_SYSINFO_H
#define APTOS_SYSINFO_H

#include "types.h"
#include "fs.h"

#define SYSINFO_MAGIC 0x49505341u  /* 'APSI' */

typedef struct PACKED {
    u32 magic;
    char os_name[32];
    char os_ver[32];
    char cpu_vendor[16];
    char cpu_model[48];
    u32 cpu_feat;
    u32 mem_total_kb;
    u32 mem_free_kb;
    u32 heap_used;
    u32 uptime_ms;
    u32 dev_count;
    char devs[8][40];
    u32 mount_count;
    fs_mountinfo_t mounts[8];
    u32 proc_active;
    u32 proc_status;
} sysinfo_t;

void sysinfo_init(void);
void sysinfo_fill(sysinfo_t *si);

#endif
