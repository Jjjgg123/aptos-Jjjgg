#ifndef APTOS_BOOTINFO_H
#define APTOS_BOOTINFO_H

#include "types.h"

#define BOOTINFO_ADDR   0x5000u
#define BOOTINFO_MAGIC  0x544F4F42u

#define E820_MAX_ENTRIES 40

typedef struct PACKED {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi_ext;
} e820_entry_t;

typedef struct PACKED {
    u32 magic;
    u32 fb_addr;
    u16 fb_pitch;
    u16 fb_width;
    u16 fb_height;
    u8  fb_bpp;
    u8  reserved0;
    u32 e820_addr;
    u32 e820_count;
    u32 kernel_lba;
    u32 kernel_sectors;
    u32 fs_lba;
    u32 boot_drive;
    u32 reserved1[3];
} bootinfo_t;

#define BOOTINFO ((bootinfo_t *)BOOTINFO_ADDR)

static inline int is_booting_from_hdd(void)
{
    return BOOTINFO->boot_drive >= 0x80;
}

#endif
