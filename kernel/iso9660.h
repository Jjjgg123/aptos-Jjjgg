#ifndef APTOS_ISO9660_H
#define APTOS_ISO9660_H

#include "types.h"
#include "fs.h"

#define ISO_SECTOR 2048

typedef struct {
    int  dev;
    u32  root_lba;
    u32  root_size;
    char label[33];
    int  mounted;
} iso_state_t;

int iso_bind(iso_state_t *st, int dev);
int iso_list(iso_state_t *st, const char *path, int index, fs_dirent_t *out);
int iso_resolve(iso_state_t *st, const char *path, fs_dirent_t *out);
int iso_read(iso_state_t *st, const char *path, void *buf, u32 max);

#endif
