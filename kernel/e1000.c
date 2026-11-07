#include "e1000.h"
#include "netdev.h"
#include "pci.h"
#include "io.h"
#include "paging.h"
#include "heap.h"
#include "klog.h"
#include "util.h"
#include "timer.h"

#define E1000_DEV_82540EM 0x100E
#define E1000_DEV_82545EM 0x100F
#define E1000_DEV_82574L  0x10D3
#define E1000_DEV_82567LF 0x10DE
#define E1000_DEV_82541PI 0x107C

/* registers (MMIO offsets) */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_RXDCTL  0x2828
#define REG_TCTL    0x0400
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_TXDCTL  0x3820
#define REG_MTA     0x5200
#define REG_RAL     0x5400
#define REG_RAH     0x5404

#define RCTL_EN     0x00000002u
#define RCTL_BAM    0x00008000u
#define RCTL_SECRC  0x04000000u
#define TCTL_EN     0x00000002u
#define TCTL_PSP    0x00000008u
#define TCTL_CT     0x00000100u  /* 0x10 << 4 */
#define TCTL_COLD   0x00040000u  /* 0x40 << 12 */

#define DESC_NUM    32
#define RX_BUF_SIZE 2048

struct rx_desc {
    u32 lo;
    u32 hi;
    u16 len;
    u16 csum;
    u8  status;
    u8  errors;
    u16 special;
};

struct tx_desc {
    u32 lo;
    u32 hi;
    u16 len;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
};

#define DESC_DD  0x01
#define DESC_EOP 0x02
#define TX_CMD   0x19  /* EOP | RS | IFCS */

static u32 mmio;
static struct rx_desc *rxr;
static u16 rx_next;
static struct tx_desc *txr;
static u8  *rxbufs[DESC_NUM];
static u8  mac[6];
static int present;
static e1000_rx_cb cb;

static inline u32 reg_read(u32 off)
{
    return *(volatile u32 *)(mmio + off);
}

static inline void reg_write(u32 off, u32 val)
{
    *(volatile u32 *)(mmio + off) = val;
}

static inline void dma_sync(void)
{
    asm volatile("wbinvd" ::: "memory");
}

static void rx_init(void)
{
    int i;
    u32 phys;

    rxr = (struct rx_desc *)kmalloc_a(16 * DESC_NUM, 16);
    memset(rxr, 0, 16 * DESC_NUM);
    phys = (u32)rxr;

    for (i = 0; i < DESC_NUM; i++) {
        rxbufs[i] = (u8 *)kmalloc_a(RX_BUF_SIZE, 16);
        rxr[i].lo = (u32)rxbufs[i];
        rxr[i].hi = 0;
        rxr[i].status = 0;
    }

    reg_write(REG_RDBAL, phys);
    reg_write(REG_RDBAH, 0);
    reg_write(REG_RDLEN, 16 * DESC_NUM);
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, DESC_NUM - 1);
    rx_next = 0;
    reg_write(REG_RXDCTL, 0);
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | 0x00000004u); /* +UP unicast promisc */
}

static void tx_init(void)
{
    u32 phys;

    txr = (struct tx_desc *)kmalloc_a(16 * DESC_NUM, 16);
    memset(txr, 0, 16 * DESC_NUM);
    phys = (u32)txr;

    reg_write(REG_TDBAL, phys);
    reg_write(REG_TDBAH, 0);
    reg_write(REG_TDLEN, 16 * DESC_NUM);
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(0x1000, 0x000C0030); /* PBA: 48KB RX / 12KB TX */
    reg_write(REG_TXDCTL, 0x01010000); /* WTHRESH=1 GRAN=1 */
    reg_write(0x0410, 0x3C0210); /* TIPG: 1000Mb/s frame spacing */
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
}

int e1000_init(e1000_rx_cb rxc)
{
    u8 bus, dev, fn;
    u32 bar;
    u32 dev_id;
    int found;

    {
        int db;
        for (db = 0; db < 32; db++) {
            u32 id = pci_read(0, db, 0, 0);
            u16 v = id & 0xFFFF;
            if (v != 0xFFFF && v != 0)
                kprintf("[net] PCI dev %02x: vendor=0x%04x device=0x%04x\n",
                        db, v, (u16)(id >> 16));
        }
    }
    found = pci_find(0x8086, 0xFF00, 0x1000, &bus, &dev, &fn, &bar);
    if (!found) {
        kprintf("[net] no Intel e1000 NIC found on PCI\n");
        return -1;
    }
    dev_id = pci_read(bus, dev, fn, 0) >> 16;
    kprintf("[net] PCI %02x:%02x.%x: Intel NIC dev=0x%04x bar0=0x%x\n",
            bus, dev, fn, dev_id, bar);

    pci_enable_master(bus, dev, fn);

    mmio = bar & ~0xFu;
    if (mmio == 0) {
        kprintf("[net] NIC has no MMIO BAR\n");
        return -1;
    }
    paging_map_nocache(mmio, 0x10000);
    kprintf("[net] MMIO mapped at 0x%x\n", mmio);

    reg_write(REG_CTRL, 0);
    reg_write(REG_IMC, 0xFFFFFFFFu);

    /* force link up + auto speed detect (required for 82540EM) */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | 0x22); /* SLU=bit1(0x02) | ASDE=bit5(0x20) */

    rx_init();
    tx_init();

    /* spin briefly for link (no timer dependency: net_init may run before PIT) */
    {
        int t;
        int up = 0;
        for (t = 0; t < 50; t++) {
            volatile int k;
            if ((reg_read(REG_STATUS) >> 1) & 1) {
                up = 1;
                break;
            }
            for (k = 0; k < 20000; k++)
                ;
        }
        kprintf("[net] link up=%d STATUS=0x%x CTRL=0x%x TCTL=0x%x RCTL=0x%x TDH=%d TDT=%d\n",
                up, reg_read(REG_STATUS), reg_read(REG_CTRL),
                reg_read(REG_TCTL), reg_read(REG_RCTL),
                (int)(reg_read(REG_TDH) & (DESC_NUM - 1)),
                (int)(reg_read(REG_TDT) & (DESC_NUM - 1)));
    }


    /* read MAC from RAL/RAH */
    {
        u32 ral = reg_read(REG_RAL);
        u32 rah = reg_read(REG_RAH);
        mac[0] = ral & 0xFF;
        mac[1] = (ral >> 8) & 0xFF;
        mac[2] = (ral >> 16) & 0xFF;
        mac[3] = (ral >> 24) & 0xFF;
        mac[4] = rah & 0xFF;
        mac[5] = (rah >> 8) & 0xFF;
    }

    cb = rxc;
    present = 1;
    /* 显式写接收地址过滤表: 保证单播(ICMP/TCP)帧能匹配入队 */
    reg_write(REG_RAL, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((u32)mac[3] << 24));
    reg_write(REG_RAH, mac[4] | (mac[5] << 8) | (0x8000u << 16)); /* AV=1 */
    kprintf("[net] e1000 ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    kprintf("[net] STATUS=0x%x (link=%u)\n", reg_read(REG_STATUS),
            (reg_read(REG_STATUS) >> 1) & 1);
    return 0;
}

int e1000_present(void)
{
    return present;
}

void e1000_mac(u8 *mac_out)
{
    memcpy(mac_out, mac, 6);
}

u32 e1000_link_status(void)
{
    if (!present)
        return 0;
    return (reg_read(REG_STATUS) >> 1) & 1;
}

void e1000_poll(void)
{
    int i;

    if (!present)
        return;

    for (i = 0; i < DESC_NUM; i++) {
        dma_sync();
        if (!(rxr[rx_next].status & DESC_DD))
            break;
        {
            static int rx_dbg;
            if (!rx_dbg) {
                rx_dbg = 1;
                kprintf("[net] first rx len=%d status=0x%x\n",
                        (int)rxr[rx_next].len, (int)rxr[rx_next].status);
            }
        }
        if (cb) {
            cb(rxbufs[rx_next], rxr[rx_next].len);
        }
        rxr[rx_next].status = 0;
        rx_next = (rx_next + 1) & (DESC_NUM - 1);
    }
    reg_write(REG_RDT, (rx_next - 1) & (DESC_NUM - 1));
}

nic_ops_t nic_e1000 = {
    e1000_init,
    e1000_send,
    e1000_poll,
    e1000_present,
    e1000_mac,
    "e1000",
};

int e1000_send(const void *buf, u32 len)
{
    u16 tdt, tdh;
    u32 spins = 0;

    if (!present)
        return -1;
    if (len > 1514)
        return -1;

    tdt = (u16)reg_read(REG_TDT) & (DESC_NUM - 1);
    tdh = (u16)reg_read(REG_TDH) & (DESC_NUM - 1);
    if (((tdt + 1) & (DESC_NUM - 1)) == tdh)
        return -1; /* ring full */

    /* Ethernet minimum frame is 60 bytes (without FCS). ARP is only 42,
     * and VBox's e1000 model drops short frames, so pad into a local copy. */
    {
        static u8 txpad[1600];
        const void *usep = buf;
        u32 xlen = len;
        if (xlen < 60) {
            memset(txpad, 0, 60);
            memcpy(txpad, buf, len);
            usep = txpad;
            xlen = 60;
        }
        txr[tdt].lo = (u32)usep;
        txr[tdt].hi = 0;
        txr[tdt].len = (u16)xlen;
    }
    txr[tdt].cmd = TX_CMD;
    txr[tdt].status = 0;

    dma_sync();
    reg_write(REG_TDT, (tdt + 1) & (DESC_NUM - 1));

    while (spins++ < 500000) {
        dma_sync();
        if (((u16)reg_read(REG_TDH) & (DESC_NUM - 1)) ==
            ((tdt + 1) & (DESC_NUM - 1))) {
            return (int)len;
        }
    }
    kprintf("[net] tx timeout len=%u spins=%u TDH=%d TDT=%d st=0x%x\n",
            len, spins, (int)(reg_read(REG_TDH) & (DESC_NUM - 1)),
            (int)(reg_read(REG_TDT) & (DESC_NUM - 1)), (int)txr[tdt].status);
    return (int)len; /* optimistic: assume sent */
}
