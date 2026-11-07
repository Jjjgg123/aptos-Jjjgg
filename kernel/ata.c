#include "ata.h"
#include "blkdev.h"
#include "io.h"
#include "klog.h"
#include "util.h"

#define CHANS 2

#define REG_DATA    0
#define REG_ERROR   1
#define REG_FEAT    1
#define REG_SECCNT  2
#define REG_LBA0    3
#define REG_LBA1    4
#define REG_LBA2    5
#define REG_DRIVE   6
#define REG_STATUS  7
#define REG_COMMAND 7

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENT    0xEC
#define CMD_IDENT_PI 0xA1
#define CMD_PACKET   0xA0

typedef struct {
    u16 io;
    u16 ctrl;
} ata_chan_t;

static const ata_chan_t chans[CHANS] = {
    { 0x1F0, 0x3F6 },
    { 0x170, 0x376 },
};

typedef struct {
    int present;
    int atapi;
    int chan;
    int slave;
    u32 sectors;
    u32 sector_size;
    char model[41];
    int blk;
} ata_dev_t;

static ata_dev_t devs[4];
static int ndevs;
static int ncd;

static void io_delay(int c)
{
    for (int i = 0; i < 4; i++)
        (void)inb(chans[c].ctrl);
}

static int wait_not_busy(int c, u32 spins)
{
    u16 io = chans[c].io;
    for (u32 i = 0; i < spins; i++) {
        u8 st = inb(io + REG_STATUS);
        if (!(st & ST_BSY))
            return 0;
    }
    return -1;
}

static int wait_drq(int c, u32 spins)
{
    u16 io = chans[c].io;
    for (u32 i = 0; i < spins; i++) {
        u8 st = inb(io + REG_STATUS);
        if (st & (ST_ERR | ST_DF))
            return -2;
        if (!(st & ST_BSY) && (st & ST_DRQ))
            return 0;
    }
    return -1;
}

static void select_drive(int c, int slave, u32 lba)
{
    u16 io = chans[c].io;
    outb(io + REG_DRIVE, (u8)((slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F)));
    io_delay(c);
}

static void select_packet_dev(int c, int slave)
{
    u16 io = chans[c].io;
    outb(io + REG_DRIVE, (u8)(slave ? 0xB0 : 0xA0));
    io_delay(c);
}

static void ident_string(u16 *id, int first, int words, char *out)
{
    int n = 0;
    for (int i = 0; i < words && n < 38; i++) {
        out[n++] = (char)(id[first + i] >> 8);
        out[n++] = (char)(id[first + i] & 0xFF);
    }
    out[n] = 0;
    for (int i = n - 1; i > 0 && out[i] == ' '; i--)
        out[i] = 0;
}

static int identify(int c, int slave, u16 *id, int *is_atapi)
{
    u16 io = chans[c].io;

    *is_atapi = 0;
    if (wait_not_busy(c, 1000000) != 0)
        return -1;

    select_drive(c, slave, 0);
    outb(io + REG_SECCNT, 0);
    outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0);
    outb(io + REG_LBA2, 0);
    outb(io + REG_COMMAND, CMD_IDENT);

    u8 st = inb(io + REG_STATUS);
    if (st == 0)
        return -1;
    if (wait_not_busy(c, 1000000) != 0)
        return -1;

    if (inb(io + REG_LBA1) != 0 || inb(io + REG_LBA2) != 0) {
        *is_atapi = 1;
        if (wait_not_busy(c, 1000000) != 0)
            return -1;
        select_packet_dev(c, slave);
        outb(io + REG_COMMAND, CMD_IDENT_PI);
        if (wait_not_busy(c, 1000000) != 0)
            return -1;
        if (wait_drq(c, 1000000) != 0)
            return -1;
        insw(io + REG_DATA, id, 256);
        return 0;
    }

    if (wait_drq(c, 1000000) != 0)
        return -1;
    insw(io + REG_DATA, id, 256);
    return 0;
}

static int pio_read(int c, int slave, u32 lba, u32 count, void *buf)
{
    u16 io = chans[c].io;
    u8 *p = (u8 *)buf;

    if (wait_not_busy(c, 4000000) != 0)
        return -1;

    select_drive(c, slave, lba);
    outb(io + REG_SECCNT, (u8)count);
    outb(io + REG_LBA0, (u8)(lba & 0xFF));
    outb(io + REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(io + REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(io + REG_COMMAND, CMD_READ);

    for (u32 i = 0; i < count; i++) {
        if (wait_drq(c, 4000000) != 0) {
            kprintf("[ata] read error at lba %d\n", lba + i);
            return -1;
        }
        insw(io + REG_DATA, p, 256);
        p += 512;
        io_delay(c);
    }
    return 0;
}

static int pio_write(int c, int slave, u32 lba, u32 count, const void *buf)
{
    u16 io = chans[c].io;
    const u8 *p = (const u8 *)buf;

    if (wait_not_busy(c, 4000000) != 0)
        return -1;

    select_drive(c, slave, lba);
    outb(io + REG_SECCNT, (u8)count);
    outb(io + REG_LBA0, (u8)(lba & 0xFF));
    outb(io + REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(io + REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(io + REG_COMMAND, CMD_WRITE);

    for (u32 i = 0; i < count; i++) {
        if (wait_drq(c, 4000000) != 0) {
            kprintf("[ata] write error at lba %d\n", lba + i);
            return -1;
        }
        outsw(io + REG_DATA, p, 256);
        p += 512;
        io_delay(c);
    }

    outb(io + REG_COMMAND, CMD_FLUSH);
    wait_not_busy(c, 4000000);
    return 0;
}

static int atapi_capacity(int c, int slave, u32 *last_lba, u32 *block_size)
{
    u16 io = chans[c].io;
    u8 packet[12];
    u8 data[8];

    if (wait_not_busy(c, 4000000) != 0)
        return -1;
    select_packet_dev(c, slave);
    outb(io + REG_FEAT, 0);
    outb(io + REG_LBA1, 0x00);
    outb(io + REG_LBA2, 0x08);
    outb(io + REG_COMMAND, CMD_PACKET);
    if (wait_drq(c, 4000000) != 0)
        return -2;

    memset(packet, 0, 12);
    packet[0] = 0x25;
    outsw(io + REG_DATA, packet, 6);

    if (wait_drq(c, 4000000) != 0) {
        kprintf("[atapi] capacity cmd failed st=0x%x err=0x%x\n", inb(io + REG_STATUS),
                inb(io + REG_ERROR));
        return -3;
    }
    insw(io + REG_DATA, data, 4);
    *last_lba = ((u32)data[0] << 24) | ((u32)data[1] << 16) |
                ((u32)data[2] << 8) | data[3];
    *block_size = ((u32)data[4] << 24) | ((u32)data[5] << 16) |
                  ((u32)data[6] << 8) | data[7];
    return 0;
}

static int atapi_test_ready(int c, int slave)
{
    u16 io = chans[c].io;
    u8 packet[12];

    if (wait_not_busy(c, 4000000) != 0)
        return -1;
    select_packet_dev(c, slave);
    outb(io + REG_FEAT, 0);
    outb(io + REG_LBA1, 0x00);
    outb(io + REG_LBA2, 0x00);
    outb(io + REG_COMMAND, CMD_PACKET);
    if (wait_drq(c, 4000000) != 0)
        return -2;
    memset(packet, 0, 12);
    outsw(io + REG_DATA, packet, 6);
    for (u32 i = 0; i < 200000; i++) {
        u8 st = inb(io + REG_STATUS);
        if (!(st & ST_BSY) && !(st & ST_DRQ))
            return (st & ST_ERR) ? -3 : 0;
    }
    return -4;
}

static int atapi_read(int c, int slave, u32 lba, u32 count, void *buf)
{
    u16 io = chans[c].io;

    for (u32 i = 0; i < count; i++) {
        u8 packet[12];

        if (wait_not_busy(c, 4000000) != 0)
            return -1;

        select_packet_dev(c, slave);
        outb(io + REG_FEAT, 0);
        outb(io + REG_LBA1, 0x00);
        outb(io + REG_LBA2, 0x08);
        outb(io + REG_COMMAND, CMD_PACKET);

        if (wait_drq(c, 4000000) != 0)
            return -1;

        memset(packet, 0, sizeof(packet));
        packet[0] = 0x28;
        u32 l = lba + i;
        packet[2] = (u8)(l >> 24);
        packet[3] = (u8)(l >> 16);
        packet[4] = (u8)(l >> 8);
        packet[5] = (u8)l;
        packet[8] = 1;
        outsw(io + REG_DATA, packet, 6);

        if (wait_drq(c, 4000000) != 0)
            return -1;

        insw(io + REG_DATA, (u8 *)buf + (u32)i * 2048u, 1024);
        io_delay(c);
    }
    return 0;
}

static int atapi_read_retry(int c, int slave, u32 lba, u32 count, void *buf)
{
    int r = atapi_read(c, slave, lba, count, buf);
    if (r != 0)
        r = atapi_read(c, slave, lba, count, buf);
    return r;
}

static ata_dev_t *by_blk(int blk)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].present && devs[i].blk == blk)
            return &devs[i];
    return NULL;
}

static int blk_read_cb(int blk, u32 lba, u32 count, void *buf)
{
    ata_dev_t *d = by_blk(blk);
    if (!d || count == 0)
        return -1;
    if (d->atapi)
        return atapi_read_retry(d->chan, d->slave, lba, count, buf);
    if (count > 255)
        return -1;
    return pio_read(d->chan, d->slave, lba, count, buf);
}

static int blk_write_cb(int blk, u32 lba, u32 count, const void *buf)
{
    ata_dev_t *d = by_blk(blk);
    if (!d || count == 0 || d->atapi || count > 255)
        return -1;
    return pio_write(d->chan, d->slave, lba, count, buf);
}

int ata_init(void)
{
    ndevs = 0;
    ncd = 0;

    for (int c = 0; c < CHANS; c++) {
        for (int s = 0; s < 2; s++) {
            ata_dev_t *d = &devs[ndevs];
            u16 id[256];
            int atapi = 0;

            if (identify(c, s, id, &atapi) != 0)
                continue;

            memset(d, 0, sizeof(*d));
            d->present = 1;
            d->atapi = atapi;
            d->chan = c;
            d->slave = s;
            d->blk = -1;
            ident_string(id, 27, 20, d->model);

            char name[8];
            if (atapi) {
                u32 last_lba = 0, bsz = 2048;
                d->sector_size = 2048;
                (void)atapi_test_ready(c, s);
                if (atapi_capacity(c, s, &last_lba, &bsz) == 0) {
                    d->sectors = last_lba + 1;
                    if (bsz)
                        d->sector_size = bsz;
                } else {
                    d->sectors = 1;
                }
                snprintf(name, sizeof(name), "cd%d", ncd++);
                d->blk = blk_register(name, BLK_CDROM, d->sectors, 2048, 1, 0,
                                      d->model, blk_read_cb, NULL);
            } else {
                d->sector_size = 512;
                if (id[83] & 0x0400) {
                    u32 hi = ((u32)id[103] << 16) | id[102];
                    u32 lo = ((u32)id[101] << 16) | id[100];
                    d->sectors = (hi != 0) ? 0x0FFFFFFFu : lo;
                } else {
                    d->sectors = ((u32)id[61] << 16) | id[60];
                }
                snprintf(name, sizeof(name), "hd%d", c * 2 + s);
                d->blk = blk_register(name, BLK_HDD, d->sectors, 512, 0, 1,
                                      d->model, blk_read_cb, blk_write_cb);
            }

            kprintf("[ata] %s: %s (%s, %d sectors x %d bytes)\n", name, d->model,
                    blk_type_name(atapi ? BLK_CDROM : BLK_HDD), d->sectors,
                    d->sector_size);
            ndevs++;
        }
    }

    if (ndevs == 0)
        kprintf("[ata] no drives found\n");
    return ndevs;
}

int ata_devices(void)
{
    return ndevs;
}
