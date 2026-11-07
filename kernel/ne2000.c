#include "netdev.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "klog.h"
#include "util.h"

/* NE2000 PCI = Realtek RTL8029AS（Bochs/QEMU 老默认、VirtualBox 可选）
 * 16KB 板载 SRAM 环形缓冲 + 远端 DMA（端口 0x10）收发。
 */
#define NE2K_VENDOR  0x10EC
#define NE2K_DEV_8029 0x8029

#define REG_CR       0x00
#define REG_PSTART   0x01
#define REG_PSTOP    0x02
#define REG_BNRY     0x03
#define REG_TPSR     0x04
#define REG_ISR      0x07
#define REG_TBCR0    0x08
#define REG_TBCR1    0x09
#define REG_RSAR0    0x08
#define REG_RSAR1    0x09
#define REG_RBCR0    0x0A
#define REG_RBCR1    0x0B
#define REG_RCR      0x0C
#define REG_TCR      0x0D
#define REG_DCR      0x0E
#define REG_IMR      0x0F
#define REG_RDCR     0x07   /* same port as ISR; mode bits select DMA dir */
#define REG_DATA     0x10
#define REG_RESET    0x1F

#define CR_PAGE0     0x20
#define CR_PAGE1     0x40
#define CR_STA       0x04
#define CR_START     0x02
#define CR_STOP      0x01
#define CR_TXP       0x24   /* page0+STA+TXP */

#define RDCR_DMA_READ  0x80  /* remote DMA read (rx) */
#define RDCR_DMA_WRITE 0x40  /* remote DMA write (tx) */

#define RX_START 0x4C   /* 16KB ring: 0x4C00..0x7FFF */
#define RX_STOP  0x80

static u16 io;
static u8  mac[6];
static int present;
static nic_rx_cb cb;
static u8  cur_page;

static inline void page0(void) { outb(io + REG_CR, CR_PAGE0 | CR_STA | CR_START); }
static inline void page1(void) { outb(io + REG_CR, CR_PAGE1 | CR_STA | CR_START); }

/* remote DMA write: dst = SRAM byte offset, len bytes from buf */
static void rw_dma_write(u16 dst, const u8 *buf, u16 len)
{
    u16 i;
    outb(io + REG_RSAR0, dst & 0xFF);
    outb(io + REG_RSAR1, (dst >> 8) & 0xFF);
    outb(io + REG_RBCR0, len & 0xFF);
    outb(io + REG_RBCR1, (len >> 8) & 0xFF);
    outb(io + REG_RDCR, RDCR_DMA_WRITE);
    for (i = 0; i < len; i++)
        outb(io + REG_DATA, buf[i]);
    /* stop DMA */
    outb(io + REG_RDCR, 0x20);
}

/* remote DMA read: src = SRAM byte offset, len bytes into buf */
static void rw_dma_read(u16 src, u8 *buf, u16 len)
{
    u16 i;
    outb(io + REG_RSAR0, src & 0xFF);
    outb(io + REG_RSAR1, (src >> 8) & 0xFF);
    outb(io + REG_RBCR0, len & 0xFF);
    outb(io + REG_RBCR1, (len >> 8) & 0xFF);
    outb(io + REG_RDCR, RDCR_DMA_READ);
    for (i = 0; i < len; i++)
        buf[i] = inb(io + REG_DATA);
    outb(io + REG_RDCR, 0x20);
}

int ne2000_init(nic_rx_cb rxc)
{
    u8 bus, dev, fn;
    u32 bar;
    u16 prom[8];
    u8  tryn;

    if (!pci_find(NE2K_VENDOR, 0xFFFF, NE2K_DEV_8029, &bus, &dev, &fn, &bar)) {
        kprintf("[net] no NE2000(8029) NIC on PCI\n");
        return -1;
    }
    io = (u16)(bar & ~0x3u);
    if (io == 0) {
        kprintf("[net] NE2000 has no I/O BAR\n");
        return -1;
    }
    pci_enable_master(bus, dev, fn);
    kprintf("[net] NE2000 %02x:%02x.%x io=0x%x\n", bus, dev, fn, io);

    /* reset */
    outb(io + REG_RESET, 0xFF);
    for (tryn = 0; tryn < 100; tryn++) {
        io_wait();
        outb(io + REG_RESET, 0x00);
        page0();
        if ((inb(io + REG_ISR) & 0x01) == 0)
            break;
    }
    page0();
    /* clear ISR */
    outb(io + REG_ISR, 0xFF);

    /* read MAC from boot PROM (remote DMA read 0x0000..0x000F, MAC in first 6) */
    {
        u8 pbuf[16];
        outb(io + REG_RSAR0, 0x00);
        outb(io + REG_RSAR1, 0x00);
        outb(io + REG_RBCR0, 16);
        outb(io + REG_RBCR1, 0x00);
        outb(io + REG_RDCR, RDCR_DMA_READ);
        for (tryn = 0; tryn < 16; tryn++)
            pbuf[tryn] = inb(io + REG_DATA);
        outb(io + REG_RDCR, 0x20);
        memcpy(mac, pbuf, 6);
        (void)prom;
    }

    /* ring config: 16KB at 0x4C00 */
    page0();
    outb(io + REG_PSTART, RX_START);
    outb(io + REG_PSTOP, RX_STOP);
    outb(io + REG_BNRY, RX_START);
    outb(io + REG_DCR, 0x48);   /* 16-bit DMA, FIFO threshold */
    outb(io + REG_TCR, 0x00);   /* loopback off */
    outb(io + REG_RCR, 0xCC);   /* promiscuous (AB|AM|APM|ARP) */
    outb(io + REG_IMR, 0x00);   /* poll mode */

    /* set MAC in PAR (page 1) */
    page1();
    {
        int i;
        for (i = 0; i < 6; i++)
            outb(io + 0x01 + i, mac[i]);
        cur_page = (u8)(RX_START + 1);
        outb(io + 0x07, cur_page); /* CURR */
    }
    page0();
    outb(io + REG_CR, CR_PAGE0 | CR_STA | CR_START);

    cb = rxc;
    present = 1;
    kprintf("[net] NE2000 ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

int ne2000_send(const void *buf, u32 len)
{
    u16 spins = 0;
    if (!present)
        return -1;
    if (len == 0 || len > 1518)
        return -1;

    /* wait for previous TX done (ISR bit0 PTX or bit1 TXE set) */
    page0();
    for (;;) {
        u8 isr = inb(io + REG_ISR);
        if (isr & 0x03)
            break;
        if (++spins > 2000000)
            return -1;
    }
    outb(io + REG_ISR, 0x03);

    /* copy frame into TX ring at 0x4000 (TPSR=0x40), remote DMA write */
    rw_dma_write(0x4000, (const u8 *)buf, (u16)len);
    page0();
    outb(io + REG_TPSR, 0x40);
    outb(io + REG_TBCR0, len & 0xFF);
    outb(io + REG_TBCR1, (len >> 8) & 0xFF);
    outb(io + REG_CR, CR_TXP); /* page0 + STA + TXP */

    /* wait until sent */
    spins = 0;
    for (;;) {
        u8 isr;
        page0();
        isr = inb(io + REG_ISR);
        if (isr & 0x03) {
            outb(io + REG_ISR, 0x03);
            break;
        }
        if (++spins > 2000000)
            break;
    }
    page0();
    outb(io + REG_CR, CR_PAGE0 | CR_STA | CR_START);
    return (int)len;
}

void ne2000_poll(void)
{
    u8 curr, bdry, next, status;
    u16 len;

    if (!present)
        return;

    page1();
    curr = inb(io + 0x07); /* CURR */
    page0();
    bdry = inb(io + REG_BNRY);

    /* frames pending while CURR != BNRY+1 */
    while ((u16)((curr + 1) & 0xFF) != bdry) {
        /* read 4-byte header at bdry page */
        u8 hdr[4];
        u16 base = (u16)bdry << 8;
        rw_dma_read(base, hdr, 4);
        status = hdr[0];
        next = hdr[1];
        len = (u16)(hdr[2] | (hdr[3] << 8));

        if (!(status & 0x01)) { /* bad frame */
            bdry = next;
            continue;
        }
        if (len && len <= 1518) {
            static u8 pkt[2048];
            u16 base2 = (u16)(bdry << 8) + 4;
            u16 part1;
            /* frame may wrap across ring end (page granularity) */
            u16 first_avail = (u16)(((u16)RX_STOP << 8) - base2);
            if (len <= first_avail) {
                rw_dma_read(base2, pkt, len);
            } else {
                part1 = first_avail;
                rw_dma_read(base2, pkt, part1);
                rw_dma_read(RX_START << 8, pkt + part1, (u16)(len - part1));
            }
            if (cb)
                cb(pkt, len);
        }
        bdry = next;
        if (next == RX_STOP)
            next = RX_START; /* page 0x80 wraps to 0x4C */
        if (next == 0x00)
            next = RX_START;
        page0();
        outb(io + REG_BNRY, bdry);
        page1();
        curr = inb(io + 0x07);
        page0();
        bdry = inb(io + REG_BNRY);
    }
}

int ne2000_present(void) { return present; }
void ne2000_mac(u8 *mac_out) { memcpy(mac_out, mac, 6); }

nic_ops_t nic_ne2000 = {
    ne2000_init,
    ne2000_send,
    ne2000_poll,
    ne2000_present,
    ne2000_mac,
    "ne2000",
};
