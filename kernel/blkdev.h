#ifndef APTOS_BLKDEV_H
#define APTOS_BLKDEV_H

#include "types.h"

#define BLK_MAX 10

#define BLK_NONE   0
#define BLK_HDD    1
#define BLK_CDROM  2
#define BLK_FLOPPY 3

typedef struct {
    char name[8];
    int  type;
    int  present;
    int  removable;
    int  writable;
    u32  sectors;
    u32  sector_size;
    char model[36];
} blk_info_t;

void  blk_init(void);
int   blk_count(void);
int   blk_find(const char *name);
const blk_info_t *blk_info(int dev);
int   blk_read(int dev, u32 lba, u32 count, void *buf);
int   blk_write(int dev, u32 lba, u32 count, const void *buf);
const char *blk_type_name(int type);

int   blk_register(const char *name, int type, u32 sectors, u32 sector_size,
                   int removable, int writable, const char *model,
                   int (*rd)(int dev, u32 lba, u32 count, void *buf),
                   int (*wr)(int dev, u32 lba, u32 count, const void *buf));

#endif
