#include "blkdev.h"
#include "klog.h"
#include "util.h"

typedef struct {
    blk_info_t info;
    int (*rd)(int dev, u32 lba, u32 count, void *buf);
    int (*wr)(int dev, u32 lba, u32 count, const void *buf);
} blk_slot_t;

static blk_slot_t slots[BLK_MAX];
static int nslots;

void blk_init(void)
{
    memset(slots, 0, sizeof(slots));
    nslots = 0;
}

int blk_register(const char *name, int type, u32 sectors, u32 sector_size,
                 int removable, int writable, const char *model,
                 int (*rd)(int dev, u32 lba, u32 count, void *buf),
                 int (*wr)(int dev, u32 lba, u32 count, const void *buf))
{
    if (nslots >= BLK_MAX)
        return -1;

    blk_slot_t *s = &slots[nslots];
    memset(s, 0, sizeof(*s));
    strncpy(s->info.name, name, sizeof(s->info.name) - 1);
    s->info.type = type;
    s->info.present = 1;
    s->info.removable = removable;
    s->info.writable = writable;
    s->info.sectors = sectors;
    s->info.sector_size = sector_size ? sector_size : 512;
    if (model)
        strncpy(s->info.model, model, sizeof(s->info.model) - 1);
    s->rd = rd;
    s->wr = wr;
    nslots++;
    return nslots - 1;
}

int blk_count(void)
{
    return nslots;
}

const blk_info_t *blk_info(int dev)
{
    if (dev < 0 || dev >= nslots)
        return NULL;
    return &slots[dev].info;
}

int blk_find(const char *name)
{
    for (int i = 0; i < nslots; i++)
        if (strcasecmp(slots[i].info.name, name) == 0)
            return i;
    return -1;
}

#define BLK_CHUNK 128

int blk_read(int dev, u32 lba, u32 count, void *buf)
{
    u32 done = 0;
    u32 ssz;

    if (dev < 0 || dev >= nslots || !slots[dev].rd)
        return -1;
    ssz = (u32)slots[dev].info.sector_size;

    while (done < count) {
        u32 n = count - done;
        if (n > BLK_CHUNK)
            n = BLK_CHUNK;
        if (slots[dev].rd(dev, lba + done, n, (u8 *)buf + (size_t)done * ssz) != 0)
            return -1;
        done += n;
    }
    return 0;
}

int blk_write(int dev, u32 lba, u32 count, const void *buf)
{
    u32 done = 0;
    u32 ssz;

    if (dev < 0 || dev >= nslots || !slots[dev].wr)
        return -1;
    if (!slots[dev].info.writable)
        return -1;
    ssz = (u32)slots[dev].info.sector_size;

    while (done < count) {
        u32 n = count - done;
        if (n > BLK_CHUNK)
            n = BLK_CHUNK;
        if (slots[dev].wr(dev, lba + done, n, (const u8 *)buf + (size_t)done * ssz) != 0)
            return -1;
        done += n;
    }
    return 0;
}

const char *blk_type_name(int type)
{
    switch (type) {
    case BLK_HDD:    return "hdd";
    case BLK_CDROM:  return "cdrom";
    case BLK_FLOPPY: return "floppy";
    default:         return "none";
    }
}
