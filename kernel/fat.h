#ifndef APTOS_FAT_H
#define APTOS_FAT_H

#include "types.h"
#include "fs.h"

#define FAT_TYPE_NONE 0
#define FAT_TYPE_12   12
#define FAT_TYPE_16   16
#define FAT_TYPE_32   32

typedef struct {
    int  dev;
    u32  base;
    int  type;
    u16  bps;
    u8   spc;
    u16  rsvd;
    u8   nfat;
    u16  root_ent;
    u32  fat_sz;
    u32  root_sectors;
    u32  root_cluster;
    u32  first_data;
    u32  clusters;
    char label[12];
    int  mounted;
} fat_state_t;

int  fat_probe(int dev, u32 base);
int  fat_bind(fat_state_t *st, int dev, u32 base);
int  fat_format(int dev, u32 base, const char *label, u32 sectors, int want_type);

int  fat_resolve(fat_state_t *st, const char *path, fs_dirent_t *out);
int  fat_list(fat_state_t *st, const char *path, int index, fs_dirent_t *out);
int  fat_read(fat_state_t *st, const char *path, void *buf, u32 max);
int  fat_write(fat_state_t *st, const char *path, const void *buf, u32 size);
int  fat_mkdir(fat_state_t *st, const char *path);
int  fat_delete(fat_state_t *st, const char *path);
int  fat_rename(fat_state_t *st, const char *from, const char *to);
void fat_stat(fat_state_t *st, fs_stat_t *out);

#endif
