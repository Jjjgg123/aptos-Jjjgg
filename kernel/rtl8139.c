#include "netdev.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "klog.h"
#include "util.h"

/* Realtek RTL8139 / RTL8139C 快速以太网（Bochs/QEMU/VirtualBox 常用 PCI 网卡） */
#define RTL_VENDOR    0x10EC
#define RTL_DEV_8139  0x8139

#define REG_MAC0      0x00
#define REG_TSD0      0x10
#define REG_TSAD0     0x20
#define REG_RBSTART   0x30
#define REG_CMD       0x37
#define REG_CAPR      0x38
#define REG_CBR       0x3A
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_CONFIG1   0x52
#define REG_CONFIG4   0x5A

#define CMD_RST       0x10
#define CMD_RE        0x08
#define CMD_TE        0x04

#define RCR_AB        0x00000008u
#define RCR_AM        0x00000004u
#define RCR_APM       0x00000002u
#define RCR_AAP       0x00000001u
#define RCR_WRAP      0x00008000u

#define RX_BUF_SIZE   8192
#define TX_DESC_NUM   4

static u16 io;
static u8 *rxbuf;
static u8  mac[6];
static int present;
static nic_rx_cb cb;
static u32 cur_tx;

/* ring read/write with wrap (buffer is RX_BUF_SIZE bytes, 8K region) */
static void ring_read(u16 off, u8 *dst, u16 len)
{
    while (len--) {
        *dst++ = rxbuf[(off++) & (RX_BUF_SIZE - 1)];
    }
}

int rtl8139_init(nic_rx_cb rxc)
{
    u8 bus, dev, fn;
    u32 bar0, bar1;
    u16 isr_try;

    if (!pci_find(RTL_VENDOR, 0xFFFF, RTL_DEV_8139, &bus, &dev, &fn, &bar0)) {
        kprintf("[net] no RTL8139 NIC on PCI\n");
        return -1;
    }
    /* RTL8139: BAR0 = MMIO, BAR1 = I/O ports (we use I/O) */
    bar1 = pci_read(bus, dev, fn, 0x14);
    io = (u16)(bar1 & ~0x3u);
    if (io == 0) {
        kprintf("[net] RTL8139 has no I/O BAR\n");
        return -1;
    }
    pci_enable_master(bus, dev, fn);
    kprintf("[net] RTL8139 %02x:%02x.%x io=0x%x bar0=0x%x\n", bus, dev, fn, io, bar0);

    /* reset */
    outb(io + REG_CMD, CMD_RST);
    for (isr_try = 0; isr_try < 100; isr_try++) {
        io_wait();
        if (!(inb(io + REG_CMD) & CMD_RST))
            break;
    }
    /* power on (CONFIG1) + I/O decode */
    outb(io + REG_CONFIG1, 0x00);

    /* read MAC */
    for (int i = 0; i < 6; i++)
        mac[i] = inb(io + REG_MAC0 + i);

    /* RX ring: 8K buffer, 16-byte aligned physical address */
    rxbuf = (u8 *)kmalloc_a(RX_BUF_SIZE + 16, 16);
    memset(rxbuf, 0, RX_BUF_SIZE + 16);
    outl(io + REG_RBSTART, (u32)rxbuf);

    /* receive config: accept all (promisc) + wrap */
    outl(io + REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);

    /* clear pending interrupts; poll mode (no IRQ) */
    outw(io + REG_ISR, 0xFFFF);
    outw(io + REG_IMR, 0x0000);

    /* start RX + TX */
    outb(io + REG_CMD, CMD_RE | CMD_TE);

    cb = rxc;
    present = 1;
    cur_tx = 0;
    kprintf("[net] RTL8139 ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

int rtl8139_send(const void *buf, u32 len)
{
    u32 tsd, spins = 0;
    if (!present)
        return -1;
    if (len == 0 || len > 1792)
        return -1;

    /* wait for a free TX descriptor (OWN cleared = done) */
    for (;;) {
        tsd = inl(io + REG_TSD0 + cur_tx * 4);
        if (!(tsd & 0x00002000u)) /* OWN bit clear -> free */
            break;
        if (++spins > 2000000)
            return -1;
    }
    outl(io + REG_TSAD0 + cur_tx * 4, (u32)buf);
    outl(io + REG_TSD0 + cur_tx * 4, len); /* len in low bits; writing sets OWN */
    cur_tx = (cur_tx + 1) & (TX_DESC_NUM - 1);

    /* wait until sent (OWN cleared again) */
    spins = 0;
    for (;;) {
        u32 d = inl(io + REG_TSD0 + ((cur_tx - 1) & (TX_DESC_NUM - 1)));
        if (!(d & 0x00002000u))
            break;
        if (++spins > 2000000)
            break;
    }
    return (int)len;
}

void rtl8139_poll(void)
{
    u16 capr, cbr;
    if (!present)
        return;

    capr = inw(io + REG_CAPR) & 0xFFF8u;
    cbr  = inw(io + REG_CBR)  & 0xFFF8u;
    if (capr == cbr)
        return;

    for (;;) {
        u8  hdr[4];
        u16 status, plen;
        ring_read(capr, hdr, 4);
        status = (u16)(hdr[0] | (hdr[1] << 8));
        plen   = (u16)(hdr[2] | (hdr[3] << 8)) - 4; /* 4-byte FCS included in len */
        (void)status;
        if (plen > 2048)
            plen = 0;
        if (plen) {
            static u8 pkt[2048];
            u16 off = (u16)((capr + 4) & (RX_BUF_SIZE - 1));
            /* handle wrap in two chunks */
            if (off + plen <= RX_BUF_SIZE) {
                ring_read(off, pkt, plen);
            } else {
                u16 part1 = (u16)(RX_BUF_SIZE - off);
                ring_read(off, pkt, part1);
                ring_read(0, pkt + part1, (u16)(plen - part1));
            }
            if (cb)
                cb(pkt, plen);
        }
        capr = (u16)((capr + plen + 4 + 3) & 0xFFF8u);
        if (capr == cbr)
            break;
    }
    outw(io + REG_CAPR, (u16)(capr - 16));
}

int rtl8139_present(void) { return present; }
void rtl8139_mac(u8 *mac_out) { memcpy(mac_out, mac, 6); }

nic_ops_t nic_rtl8139 = {
    rtl8139_init,
    rtl8139_send,
    rtl8139_poll,
    rtl8139_present,
    rtl8139_mac,
    "rtl8139",
};
