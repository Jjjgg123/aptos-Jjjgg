#include "floppy.h"
#include "blkdev.h"
#include "io.h"
#include "klog.h"
#include "util.h"

#define DMA_ADDR2 0x04
#define DMA_CNT2  0x05
#define DMA_MASK  0x0A
#define DMA_MODE  0x0B
#define DMA_FF    0x0C
#define DMA_PAGE2 0x81

#define FDC_DOR   0x3F2
#define FDC_MSR   0x3F4
#define FDC_FIFO  0x3F5
#define FDC_DIR   0x3F7

#define CMD_READ      0x46
#define CMD_WRITE     0x45
#define CMD_SEEK      0x0F
#define CMD_RECAL     0x07
#define CMD_SENSE_INT 0x08
#define CMD_SPECIFY   0x03

#define SPT      18
#define HEADS    2
#define CYLS     80
#define SECTORS  (SPT * HEADS * CYLS)

static int present;
static u8 cur_cyl = 0xFF;

static void ms_delay(u32 ms)
{
    for (u32 i = 0; i < ms; i++)
        for (volatile u32 j = 0; j < 20000; j++)
            ;
}

static int msr_wait(int dio, u32 spins)
{
    for (u32 i = 0; i < spins; i++) {
        u8 msr = inb(FDC_MSR);
        if ((msr & 0x80) && (((msr >> 6) & 1) == (u32)dio))
            return 0;
    }
    return -1;
}

static int fdc_send(u8 v)
{
    if (msr_wait(0, 200000) != 0)
        return -1;
    outb(FDC_FIFO, v);
    return 0;
}

static int fdc_recv(u8 *v)
{
    if (msr_wait(1, 200000) != 0)
        return -1;
    *v = inb(FDC_FIFO);
    return 0;
}

static u8 dma_buf[512] __attribute__((aligned(512)));

static void motor(int on)
{
    outb(FDC_DOR, (u8)(0x0C | 0x08 | (on ? 0x10 : 0)));
}

static void dma_setup(void *buf, u32 len, int to_memory)
{
    u32 addr = (u32)buf;
    u8 mode = (u8)(0x40 | 0x02 | (to_memory ? 0x08 : 0x04));

    outb(DMA_FF, 0);
    outb(DMA_MASK, 0x06);
    outb(DMA_MODE, mode);
    outb(DMA_ADDR2, (u8)(addr & 0xFF));
    outb(DMA_ADDR2, (u8)((addr >> 8) & 0xFF));
    outb(DMA_PAGE2, (u8)((addr >> 16) & 0xFF));
    outb(DMA_CNT2, (u8)((len - 1) & 0xFF));
    outb(DMA_CNT2, (u8)(((len - 1) >> 8) & 0xFF));
    outb(DMA_MASK, 0x02);
}

static int sense_int(u8 *st0, u8 *cyl)
{
    if (fdc_send(CMD_SENSE_INT) != 0)
        return -1;
    if (fdc_recv(st0) != 0)
        return -1;
    if (fdc_recv(cyl) != 0)
        return -1;
    return 0;
}

static int fdc_reset(void)
{
    outb(FDC_DOR, 0x00);
    ms_delay(2);
    outb(FDC_DOR, 0x0C);
    ms_delay(5);

    for (int i = 0; i < 200; i++) {
        if (inb(FDC_MSR) & 0x80)
            break;
        ms_delay(1);
    }

    for (int i = 0; i < 4; i++) {
        u8 st0, cyl;
        if (sense_int(&st0, &cyl) != 0)
            return -1;
    }

    if (fdc_send(CMD_SPECIFY) != 0)
        return -1;
    if (fdc_send(0xDF) != 0)
        return -1;
    if (fdc_send(0x02) != 0)
        return -1;
    return 0;
}

static int fdc_seek(u8 cyl)
{
    u8 st0, c;

    if (cur_cyl == cyl)
        return 0;
    if (fdc_send(CMD_SEEK) != 0)
        return -1;
    if (fdc_send(0x00) != 0)
        return -1;
    if (fdc_send(cyl) != 0)
        return -1;

    for (int i = 0; i < 200; i++) {
        if (!(inb(FDC_MSR) & 0x10))
            break;
        ms_delay(1);
    }
    if (sense_int(&st0, &c) != 0)
        return -1;
    cur_cyl = cyl;
    return 0;
}

static void lba_to_chs(u32 lba, u8 *cyl, u8 *head, u8 *sec)
{
    *sec = (u8)(lba % SPT) + 1;
    *head = (u8)((lba / SPT) % HEADS);
    *cyl = (u8)(lba / (SPT * HEADS));
}

static int fdc_transfer(u32 lba, void *buf, int write)
{
    u8 cyl, head, sec;
    u8 st0, st1, st2, c, h, r, n;

    lba_to_chs(lba, &cyl, &head, &sec);
    if (cyl >= CYLS)
        return -1;

    motor(1);
    if (fdc_seek(cyl) != 0)
        return -1;

    if (write)
        memcpy(dma_buf, buf, 512);
    dma_setup(dma_buf, 512, !write);

    if (fdc_send((u8)(write ? CMD_WRITE : CMD_READ)) != 0)
        return -1;
    if (fdc_send((u8)((head << 2) | 0x00)) != 0)
        return -1;
    if (fdc_send(cyl) != 0)
        return -1;
    if (fdc_send(head) != 0)
        return -1;
    if (fdc_send(sec) != 0)
        return -1;
    if (fdc_send(2) != 0)
        return -1;
    if (fdc_send(SPT) != 0)
        return -1;
    if (fdc_send(0x1B) != 0)
        return -1;
    if (fdc_send(0xFF) != 0)
        return -1;

    for (u32 i = 0; i < 4000000; i++) {
        u8 msr = inb(FDC_MSR);
        if ((msr & 0xF0) == 0xD0)
            break;
        if (!(msr & 0x10))
            break;
    }

    if (fdc_recv(&st0) != 0 || fdc_recv(&st1) != 0 || fdc_recv(&st2) != 0 ||
        fdc_recv(&c) != 0 || fdc_recv(&h) != 0 || fdc_recv(&r) != 0 ||
        fdc_recv(&n) != 0) {
        kprintf("[fdc] no result phase msr=0x%x\n", inb(FDC_MSR));
        return -1;
    }

    if (st0 & 0xC0) {
        kprintf("[fdc] error st0=0x%x st1=0x%x st2=0x%x c=%d h=%d r=%d\n", st0, st1, st2,
                c, h, r);
        return -1;
    }

    if (!write)
        memcpy(buf, dma_buf, 512);
    return 0;
}

static int floppy_read_cb(int dev, u32 lba, u32 count, void *buf)
{
    (void)dev;
    u8 *p = (u8 *)buf;
    for (u32 i = 0; i < count; i++) {
        if (fdc_transfer(lba + i, p + i * 512, 0) != 0)
            return -1;
    }
    return 0;
}

static int floppy_write_cb(int dev, u32 lba, u32 count, const void *buf)
{
    (void)dev;
    const u8 *p = (const u8 *)buf;
    for (u32 i = 0; i < count; i++) {
        if (fdc_transfer(lba + i, (void *)(p + i * 512), 1) != 0)
            return -1;
    }
    return 0;
}

int floppy_init(void)
{
    u8 probe[512];
    int r;

    present = 0;
    cur_cyl = 0xFF;

    if (fdc_reset() != 0) {
        kprintf("[fdc] no floppy controller\n");
        return -1;
    }

    motor(1);
    ms_delay(30);
    r = fdc_transfer(0, probe, 0);
    if (r != 0) {
        motor(0);
        kprintf("[fdc] controller present, no disk in drive 0\n");
        return -1;
    }

    present = 1;
    blk_register("fd0", BLK_FLOPPY, SECTORS, 512, 1, 1, "1.44M floppy",
                 floppy_read_cb, floppy_write_cb);
    kprintf("[fdc] fd0: 1.44M floppy, %d sectors\n", SECTORS);
    return 0;
}
