#include "netdev.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "klog.h"
#include "util.h"
#include "paging.h"

/* legacy virtio-net (PCI 1AF4:1000), I/O BAR interface.
 * No PHY / link negotiation: the device is always "connected", which is exactly
 * why it is the reliable path inside VirtualBox (whose e1000 model never reports
 * link up for us). e1000 stays as the real-hardware path. */

#define VIRTIO_VENDOR  0x1AF4
#define VIRTIO_DEV_NET 0x1000

#define VIO_HOST_FEATURES  0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN      0x08
#define VIO_QUEUE_SIZE     0x0C
#define VIO_QUEUE_SELECT   0x0E
#define VIO_QUEUE_NOTIFY   0x10
#define VIO_STATUS         0x12
#define VIO_ISR            0x13
#define VIO_CONFIG         0x14

#define VIO_S_ACK       0x01
#define VIO_S_DRIVER    0x02
#define VIO_S_DRIVER_OK 0x04
#define VIO_S_FEAT_OK   0x08
#define VIO_S_FAILED    0x40

#define VF_MAC (1u << 5)

#define Q_RX 0
#define Q_TX 1
#define QNUM 2

#define RX_BUF_SZ 2048
#define MAX_RX_DESC 1024

/* virtqueue descriptor: 16 bytes, physical addresses split to avoid u64 packing */
struct vq_desc {
    u32 addr;
    u32 addr_hi;
    u32 len;
    u16 flags;
    u16 next;
};

struct vq_avail {
    u16 flags;
    u16 idx;
    u16 ring[1];
};

struct vq_used_elem {
    u32 id;
    u32 len;
};

struct vq_used {
    u16 flags;
    u16 idx;
    struct vq_used_elem ring[1];
};

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

static u32 io_base;
static u8  mac[6];
static int present;
static nic_rx_cb cb;

static u8 *q_base[QNUM];
static u32 q_pfn[QNUM];
static u16 q_size[QNUM];
static u16 q_lastused[QNUM];
static u32 q_used_off[QNUM];
static u16 tx_next_desc;
static u16 tx_avail_idx;
static u16 rx_avail_idx;
static u8 *rxbuf[MAX_RX_DESC];
static int poll_seen;

static inline u32 vq_desc_off(int q) { (void)q; return 0; }

static struct vq_desc *desc_of(int q)
{
    return (struct vq_desc *)q_base[q];
}

static struct vq_avail *avail_of(int q)
{
    return (struct vq_avail *)(q_base[q] + 16 * q_size[q]);
}

static struct vq_used *used_of(int q)
{
    return (struct vq_used *)(q_base[q] + q_used_off[q]);
}

static void notify(int q)
{
    outw(io_base + VIO_QUEUE_NOTIFY, (u16)q);
}

/* allocate one virtqueue: desc + avail + used, used 4K-aligned after avail */
static int queue_setup(int q, u16 *size_out)
{
    u16 qs;
    u32 desc_bytes;
    u32 avail_bytes;
    u32 used_off;
    u32 total;
    u8 *base;

    outw(io_base + VIO_QUEUE_SELECT, (u16)q);
    qs = inw(io_base + VIO_QUEUE_SIZE);
    if (qs < 4 || qs > 1024) {
        kprintf("[virtio] queue %d bad size %u\n", q, qs);
        return -1;
    }

    desc_bytes = 16u * qs;
    avail_bytes = 4u + 2u * qs;
    used_off = (desc_bytes + avail_bytes + 4095u) & ~4095u;
    total = used_off + 4u + 8u * qs;

    base = (u8 *)kmalloc_a(total, 4096);
    if (!base)
        return -1;
    memset(base, 0, total);

    q_base[q] = base;
    q_size[q] = qs;
    q_used_off[q] = used_off;
    q_pfn[q] = (u32)base >> 12;
    q_lastused[q] = 0;

    /* legacy interface: PFN = phys >> 12 */
    outl(io_base + VIO_QUEUE_PFN, q_pfn[q]);
    *size_out = qs;
    return 0;
}

static void rx_prefill(void)
{
    int i;
    u16 qs = q_size[Q_RX];
    int n = (qs > MAX_RX_DESC) ? MAX_RX_DESC : (int)qs;
    struct vq_desc *d = desc_of(Q_RX);
    struct vq_avail *a = avail_of(Q_RX);

    for (i = 0; i < n; i++) {
        rxbuf[i] = (u8 *)kmalloc_a(RX_BUF_SZ, 16);
        if (!rxbuf[i])
            break;
        d[i].addr = (u32)rxbuf[i];
        d[i].addr_hi = 0;
        d[i].len = RX_BUF_SZ;
        d[i].flags = VRING_DESC_F_WRITE;
        d[i].next = 0;
        a->ring[i] = (u16)i;
    }
    rx_avail_idx = (u16)n;
    a->idx = rx_avail_idx;
}

void virtio_poll(void)
{
    int guard = 0;
    u16 qs;
    struct vq_used *u;

    if (!present)
        return;
    qs = q_size[Q_RX];
    u = used_of(Q_RX);

    if (!poll_seen) {
        poll_seen = 1;
        kprintf("[virtio] first poll rx used=%u last=%u\n", u->idx, q_lastused[Q_RX]);
    }
    while (q_lastused[Q_RX] != u->idx && guard++ < 64) {
        u16 slot = (u16)(q_lastused[Q_RX] % qs);
        u32 id = u->ring[slot].id;
        u32 len = u->ring[slot].len;
        struct vq_avail *a;
        struct vq_desc *d;

        if (id < qs && len > 0 && len <= RX_BUF_SZ && cb)
            cb(rxbuf[id], (u16)len);

        /* hand the buffer back */
        d = desc_of(Q_RX);
        a = avail_of(Q_RX);
        d[id].addr = (u32)rxbuf[id];
        d[id].addr_hi = 0;
        d[id].len = RX_BUF_SZ;
        d[id].flags = VRING_DESC_F_WRITE;
        d[id].next = 0;
        a->ring[rx_avail_idx % qs] = (u16)id;
        rx_avail_idx++;
        a->idx = rx_avail_idx;

        q_lastused[Q_RX]++;
    }
    if (guard)
        notify(Q_RX);
}

int virtio_send(const void *buf, u32 len)
{
    u16 qs;
    struct vq_desc *d;
    struct vq_avail *a;
    u16 idx;

    if (!present)
        return -1;
    if (len == 0 || len > 1500)
        return -1;

    qs = q_size[Q_TX];
    d = desc_of(Q_TX);
    a = avail_of(Q_TX);
    idx = tx_next_desc;

    d[idx].addr = (u32)buf;
    d[idx].addr_hi = 0;
    d[idx].len = len;
    d[idx].flags = 0;
    d[idx].next = 0;

    a->ring[tx_avail_idx % qs] = idx;
    tx_avail_idx++;
    a->idx = tx_avail_idx;
    tx_next_desc = (u16)((idx + 1) % qs);

    notify(Q_TX);
    if ((tx_avail_idx & 0x1F) == 1)
        kprintf("[virtio] tx used=%u sent=%u st=%x avail_idx=%u\n",
                used_of(Q_TX)->idx, tx_avail_idx, inb(io_base + VIO_STATUS), avail_of(Q_TX)->idx);
    return (int)len;
}

void virtio_mac(u8 *out)
{
    memcpy(out, mac, 6);
}

int virtio_present(void)
{
    return present;
}

int virtio_net_init(nic_rx_cb rxc)
{
    u8 bus, dev, fn;
    u32 bar;
    u16 qs;
    u32 feat;

    if (!pci_find(VIRTIO_VENDOR, 0xFFFF, VIRTIO_DEV_NET, &bus, &dev, &fn, &bar)) {
        kprintf("[virtio] no virtio-net on PCI\n");
        return -1;
    }
    kprintf("[virtio] found %02x:%02x.%x bar0=0x%x\n", bus, dev, fn, bar);

    pci_enable_master(bus, dev, fn);
    io_base = bar & ~0x3u;
    if (io_base == 0) {
        kprintf("[virtio] BAR0 is not an I/O region\n");
        return -1;
    }

    /* reset then driver init sequence */
    outb(io_base + VIO_STATUS, 0);
    outb(io_base + VIO_STATUS, VIO_S_ACK);
    outb(io_base + VIO_STATUS, VIO_S_ACK | VIO_S_DRIVER);

    feat = inl(io_base + VIO_HOST_FEATURES);
    kprintf("[virtio] host features=0x%x\n", feat);
    outl(io_base + VIO_GUEST_FEATURES, VF_MAC);
    outb(io_base + VIO_STATUS, VIO_S_ACK | VIO_S_DRIVER | VIO_S_FEAT_OK);
    if (!(inb(io_base + VIO_STATUS) & VIO_S_FEAT_OK)) {
        kprintf("[virtio] feature negotiation rejected\n");
        return -1;
    }

    if (queue_setup(Q_RX, &qs) != 0)
        return -1;
    if (queue_setup(Q_TX, &qs) != 0)
        return -1;
    kprintf("[virtio] rx qsize=%u pfn=0x%x used_off=%u\n", q_size[Q_RX], q_pfn[Q_RX], q_used_off[Q_RX]);
    kprintf("[virtio] tx qsize=%u pfn=0x%x used_off=%u\n", q_size[Q_TX], q_pfn[Q_TX], q_used_off[Q_TX]);

    /* config space (legacy): MAC at 0x14 */
    mac[0] = inb(io_base + VIO_CONFIG + 0);
    mac[1] = inb(io_base + VIO_CONFIG + 1);
    mac[2] = inb(io_base + VIO_CONFIG + 2);
    mac[3] = inb(io_base + VIO_CONFIG + 3);
    mac[4] = inb(io_base + VIO_CONFIG + 4);
    mac[5] = inb(io_base + VIO_CONFIG + 5);

    cb = rxc;
    present = 1;

    outb(io_base + VIO_STATUS, VIO_S_ACK | VIO_S_DRIVER | VIO_S_FEAT_OK | VIO_S_DRIVER_OK);
    rx_prefill();
    notify(Q_RX);

    kprintf("[virtio] status=%x pfn_rb rx=%x tx=%x sel_rb=%x\n",
            inb(io_base + VIO_STATUS),
            inl(io_base + VIO_QUEUE_PFN),
            inl(io_base + VIO_QUEUE_PFN),
            inw(io_base + VIO_QUEUE_SELECT));
    kprintf("[virtio] ready MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

nic_ops_t nic_virtio = {
    virtio_net_init,
    virtio_send,
    virtio_poll,
    virtio_present,
    virtio_mac,
    "virtio-net",
};
