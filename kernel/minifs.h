#ifndef APTOS_MINIFS_H
#define APTOS_MINIFS_H

#include "types.h"
#include "fs.h"

#define MFS_MAGIC "MINIFS02"

typedef struct PACKED {
    char magic[8];
    u32  block_size;
    u32  total_blocks;
    u32  bitmap_start;
    u32  bitmap_blocks;
    u32  dir_start;
    u32  dir_blocks;
    u32  data_start;
    u32  data_blocks;
    u32  max_entries;
    u32  free_blocks;
    char label[32];
    u8   reserved[512 - 80];
} mfs_super_t;

typedef struct {
    int         dev;
    u32         base;
    mfs_super_t super;
    u8         *bitmap;
    int         mounted;
} mfs_state_t;

void mfs_use(mfs_state_t *st);
int  mfs_bind(mfs_state_t *st, int dev, u32 base);
int  mfs_format(int dev, u32 base, const char *label, u32 total_blocks);

int  mfs_mounted(void);
int  mfs_resolve(const char *path, fs_dirent_t *out);
int  mfs_exists(const char *path);
int  mfs_is_dir(const char *path);
int  mfs_list(const char *path, int index, fs_dirent_t *out);
int  mfs_dir_scan(const char *path, int (*cb)(const fs_dirent_t *, void *), void *ctx);
int  mfs_count(const char *path);
int  mfs_read(const char *path, void *buf, u32 max);
int  mfs_write(const char *path, const void *buf, u32 size);
int  mfs_mkdir(const char *path);
int  mfs_delete(const char *path);
int  mfs_rename(const char *from, const char *to);
void mfs_stat(fs_stat_t *out);
int  mfs_count_kind(const char *path, u32 *files, u32 *dirs);
u32  mfs_total_kb(void);
u32  mfs_free_kb(void);
const char *mfs_volume_label(void);

#endif
