#ifndef APTOS_FS_H
#define APTOS_FS_H

#include "types.h"

#define FS_MAX_NAME 39
#define FS_FLAG_DIR 1

typedef struct PACKED {
    char name[40];
    u32  size;
    u32  first_block;
    u32  flags;
    u32  mdate;
    u32  mtime;
    u32  reserved;
} fs_dirent_t;

typedef struct {
    u32 total_kb;
    u32 free_kb;
    u32 files;
    u32 dirs;
} fs_stat_t;

#define FS_TYPE_NONE    0
#define FS_TYPE_MINIFS  1
#define FS_TYPE_ISO9660 2
#define FS_TYPE_FAT12   3
#define FS_TYPE_FAT16   4
#define FS_TYPE_FAT32   5

typedef struct {
    char point[32];
    char dev[8];
    int  type;
    char label[32];
    u32  total_kb;
    u32  free_kb;
    int  writable;
} fs_mountinfo_t;

int  fs_init(void);
int  fs_mounted(void);

int  fs_mount(const char *devname, const char *point);
int  fs_umount(const char *point);
int  fs_format(const char *devname, const char *label);
int  fs_format_fat(const char *devname, const char *label, int want_type);
int  fs_install(int dev, u32 base, const char *label);
int  fs_install_fat(int dev, u32 base, const char *label, int want_type);
int  fs_install_root(const char *devname);
int  fs_mounts(fs_mountinfo_t *out, int max);
const char *fs_type_name(int type);

int  fs_resolve(const char *path, fs_dirent_t *out);
int  fs_exists(const char *path);
int  fs_is_dir(const char *path);
int  fs_list(const char *path, int index, fs_dirent_t *out);
int  fs_dir_scan(const char *path, int (*cb)(const fs_dirent_t *, void *), void *ctx);
int  fs_count(const char *path);

int  fs_read(const char *path, void *buf, u32 max);
int  fs_write(const char *path, const void *buf, u32 size);
int  fs_mkdir(const char *path);
int  fs_delete(const char *path);
int  fs_rename(const char *from, const char *to);

void fs_stat(fs_stat_t *out);
int  fs_count_kind(const char *path, u32 *files, u32 *dirs);

u32  fs_total_kb(void);
u32  fs_free_kb(void);
const char *fs_volume_label(void);

void fs_path(char *out, u32 outsz, const char *cwd, const char *arg);
const char *fs_basename(const char *path);
void fs_parent_path(const char *path, char *out, u32 outsz);

#endif
