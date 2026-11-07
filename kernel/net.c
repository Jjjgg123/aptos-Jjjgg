#include "net.h"
#include "netdev.h"
#include "e1000.h"
#include "io.h"
#include "heap.h"
#include "klog.h"
#include "util.h"
#include "timer.h"

/* ------------------------------------------------------------------ */
/*  configuration (VBox NAT defaults) - stored little-endian u32      */
/*  so that memcpy'ing the u32 yields wire bytes a.b.c.d              */
/* ------------------------------------------------------------------ */
static u8  my_mac[6];
static u32 my_ip   = 0x0F02000Au; /* VBox NAT */
static u32 my_gw   = 0x0202000Au; /* VBox NAT */
static u32 my_dns  = 0x0302000Au; /* VBox NAT */
static u32 my_mask = 0x00FFFFFFu; /* 255.255.255.0 */
static int up;

/* selected NIC driver (set by net_init) */
nic_ops_t *g_nic = NULL;

/* static TX buffers (e1000_send is synchronous) */
static u8 tx_arp[1518];
static u8 tx_ip[1518];
static u8 tx_icmp[1518];
static u8 tx_tcp[1518];

#define ETH_ARP 0x0806
#define ETH_IP  0x0800
#define IP_ICMP 1
#define IP_UDP  17
#define IP_TCP  6

struct eth_hdr {
    u8  dst[6];
    u8  src[6];
    u16 type;   /* wire order; compare after ntohs */
} __attribute__((packed));
struct ip_hdr {
    u8  ver_ihl;
    u8  tos;
    u16 tot_len;
    u16 id;
    u16 frag;
    u8  ttl;
    u8  proto;
    u16 csum;
    u32 src;
    u32 dst;
} __attribute__((packed));
struct arp_hdr {
    u16 htype;
    u16 ptype;
    u8  hlen;
    u8  plen;
    u16 op;
    u8  sha[6];
    u32 spa;
    u8  tha[6];
    u32 tpa;
} __attribute__((packed));
struct udp_hdr {
    u16 sport;
    u16 dport;
    u16 len;
    u16 csum;
} __attribute__((packed));
struct tcp_hdr {
    u16 sport;
    u16 dport;
    u32 seq;
    u32 ack;
    u8  off_flags;
    u8  flags;
    u16 win;
    u16 csum;
    u16 urg;
} __attribute__((packed));

/* helpers: wire (big-endian) 16-bit access */
static inline u16 rd16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}
static inline void wr16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

/* checksum over wire bytes; returns the big-endian value */
static u16 chksum16(const void *d, u32 len)
{
    const u8 *p = (const u8 *)d;
    u32 sum = 0;
    while (len >= 2) {
        sum += rd16(p);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u16)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* forward decls */
void tcp_handle(const struct ip_hdr *ip, const u8 *payload, u16 plen);

/* ------------------------------------------------------------------ */
/*  ARP                                                               */
/* ------------------------------------------------------------------ */
static u32 arp_ip[8];
static u8  arp_mac[8][6];
static u8  arp_valid[8];
static u32 arp_wait_ip;
static volatile int arp_wait_done;

/* ICMP ping state */
static u16 ping_id = 0x5350;
static u16 ping_seq;
static volatile int ping_done;
static u32 ping_rtt;
static u16 ping_want_seq;
static u32 ping_t0;

static void arp_cache(u32 ip, const u8 *mac)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (arp_valid[i] && arp_ip[i] == ip) {
            memcpy(arp_mac[i], mac, 6);
            return;
        }
    }
    for (i = 0; i < 8; i++) {
        if (!arp_valid[i]) {
            arp_valid[i] = 1;
            arp_ip[i] = ip;
            memcpy(arp_mac[i], mac, 6);
            return;
        }
    }
    /* overwrite oldest */
    arp_valid[0] = 1;
    arp_ip[0] = ip;
    memcpy(arp_mac[0], mac, 6);
}

static int arp_lookup(u32 ip, u8 *mac)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (arp_valid[i] && arp_ip[i] == ip) {
            memcpy(mac, arp_mac[i], 6);
            return 0;
        }
    }
    return -1;
}

static void arp_send_request(u32 tpa)
{
    u8 *p = tx_arp;
    struct eth_hdr *eth = (struct eth_hdr *)p;
    struct arp_hdr *arp = (struct arp_hdr *)(p + 14);
    memset(p, 0, 1518);
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, my_mac, 6);
    wr16((u8 *)&eth->type, ETH_ARP);
    wr16((u8 *)&arp->htype, 1);
    wr16((u8 *)&arp->ptype, 0x0800);
    arp->hlen = 6;
    arp->plen = 4;
    wr16((u8 *)&arp->op, 1);
    memcpy(arp->sha, my_mac, 6);
    arp->spa = my_ip;
    memset(arp->tha, 0, 6);
    arp->tpa = tpa;
    g_nic->send(p, 14 + 28);
}

/* gratuitous ARP 通告: 广播声明 our IP -> MAC, 让宿主/网关学到地址,
 * 规避"桥接回环导致宿主->VM 广播收不到"的单向问题 */
void arp_announce(void)
{
    int k;
    if (!g_nic)
        return;
    for (k = 0; k < 3; k++) {
        u8 *p = tx_arp;
        struct eth_hdr *eth = (struct eth_hdr *)p;
        struct arp_hdr *r = (struct arp_hdr *)(p + 14);
        memset(p, 0, 14 + 28);
        memset(eth->dst, 0xFF, 6);
        memcpy(eth->src, my_mac, 6);
        wr16((u8 *)&eth->type, ETH_ARP);
        wr16((u8 *)&r->htype, 1);
        wr16((u8 *)&r->ptype, 0x0800);
        r->hlen = 6;
        r->plen = 4;
        wr16((u8 *)&r->op, 2); /* ARP reply 通告 */
        memcpy(r->sha, my_mac, 6);
        r->spa = my_ip;
        memset(r->tha, 0xFF, 6);
        r->tpa = my_ip;
        g_nic->send(p, 14 + 28);
    }
    kprintf("[net] gratuitous ARP sent x3\n");
}

static void arp_handle(const u8 *pkt, u16 len)
{
    const struct arp_hdr *arp;
    if (len < 14 + 28)
        return;
    arp = (const struct arp_hdr *)(pkt + 14);
    if (rd16((const u8 *)&arp->htype) != 1)
        return;
    if (rd16((const u8 *)&arp->op) == 1) { /* request for us */
        if (arp->tpa == my_ip) {
            u8 *p = tx_arp;
            struct eth_hdr *eth = (struct eth_hdr *)p;
            struct arp_hdr *r = (struct arp_hdr *)(p + 14);
            memcpy(eth->dst, pkt + 6, 6);
            memcpy(eth->src, my_mac, 6);
            wr16((u8 *)&eth->type, ETH_ARP);
            wr16((u8 *)&r->htype, 1);
            wr16((u8 *)&r->ptype, 0x0800);
            r->hlen = 6;
            r->plen = 4;
            wr16((u8 *)&r->op, 2);
            memcpy(r->sha, my_mac, 6);
            r->spa = my_ip;
            memcpy(r->tha, arp->sha, 6);
            r->tpa = arp->spa;
            kprintf("[net] ARP reply sent\n");
            g_nic->send(p, 14 + 28);
        }
    } else if (rd16((const u8 *)&arp->op) == 2) { /* reply */
        arp_cache(arp->spa, arp->sha);
        if (arp_wait_ip == arp->spa)
            arp_wait_done = 1;
    }
}

/* blocking resolve; timeout_ms per attempt */
static int arp_resolve(u32 ip, u8 *mac, u32 timeout_ms)
{
    int attempt;
    if (ip == 0xFFFFFFFFu) { /* broadcast */
        memset(mac, 0xFF, 6);
        return 0;
    }
    if (arp_lookup(ip, mac) == 0)
        return 0;
    for (attempt = 0; attempt < 3; attempt++) {
        u32 t0 = timer_uptime_ms();
        arp_wait_ip = ip;
        arp_wait_done = 0;
        arp_send_request(ip);
        kprintf("[dbg] arp: request sent for %d.%d.%d.%d attempt %d\n", (u8)ip, (u8)(ip>>8), (u8)(ip>>16), (u8)(ip>>24), attempt);
        while (timer_uptime_ms() - t0 < timeout_ms) {
            net_poll();
            if (arp_wait_done && arp_wait_ip == ip) {
                kprintf("[dbg] arp: reply got\n");
                if (arp_lookup(ip, mac) == 0)
                    return 0;
            }
        }
        kprintf("[dbg] arp: attempt %d timeout\n", attempt);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  IP / ICMP / UDP                                                    */
/* ------------------------------------------------------------------ */
static u16 ip_id;

static int ip_send(u32 dst, u8 proto, const void *payload, u16 plen)
{
    u8 mac[6];
    u8 *p = tx_ip;
    struct eth_hdr *eth = (struct eth_hdr *)p;
    struct ip_hdr *ip = (struct ip_hdr *)(p + 14);
    u16 tot = 20 + plen;

    if (arp_resolve((dst & my_mask) == (my_ip & my_mask) ? dst : my_gw,
                    mac, 3000) < 0) {
        return -1;
    }
    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, my_mac, 6);
    wr16((u8 *)&eth->type, ETH_IP);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    wr16((u8 *)&ip->tot_len, tot);
    wr16((u8 *)&ip->id, ip_id++);
    wr16((u8 *)&ip->frag, 0);
    ip->ttl = 64;
    ip->proto = proto;
    wr16((u8 *)&ip->csum, 0);
    ip->src = my_ip;
    ip->dst = dst;
    wr16((u8 *)&ip->csum, chksum16(ip, 20));

    if (plen)
        memcpy(p + 14 + 20, payload, plen);

    g_nic->send(p, 14 + tot);
    return 0;
}

/* icmp entry log */static void icmp_handle(const struct ip_hdr *ip, const u8 *payload, u16 plen)
{
    kprintf("[dbg] icmp: type=%u plen=%u\n", payload[0], plen);
    u8 *p = tx_icmp;
    if (plen < 8)
        return;
    if (payload[0] == 0) { /* echo reply: match our ping */
        if (rd16(payload + 4) == ping_id && rd16(payload + 6) == ping_want_seq) {
            ping_rtt = timer_uptime_ms() - ping_t0;
            ping_done = 1;
        }
        return;
    }
    if (payload[0] != 8) /* echo request */
        return;
    memcpy(p, payload, plen);
    p[0] = 0; /* echo reply */
    p[1] = 0;
    wr16(p + 2, 0);
    wr16(p + 2, chksum16(p, plen));
    ip_send(ip->src, IP_ICMP, p, plen);
}

/* --- DNS client --- */
static u16 dns_sport = 0x1234;
static u16 dns_xid;
static u8  dns_rx[512];
static u32 dns_rx_len;
static volatile int dns_done;

static void udp_handle(const struct ip_hdr *ip, const u8 *payload, u16 plen)
{
    const struct udp_hdr *udp = (const struct udp_hdr *)payload;
    u16 dport, sport;
    if (plen < 8)
        return;
    dport = rd16((const u8 *)&udp->dport);
    sport = rd16((const u8 *)&udp->sport);
    if (dport == dns_sport && sport == 53 && plen > 8) {
        u32 n = plen - 8;
        if (n > sizeof(dns_rx))
            n = sizeof(dns_rx);
        memcpy(dns_rx, payload + 8, n);
        dns_rx_len = n;
        dns_done = 1;
    }
}

static void ip_handle(const u8 *pkt, u16 len)
{
    const struct ip_hdr *ip;
    const u8 *payload;
    u16 tot, ihl;
    if (len < 14 + 20)
        return;
    ip = (const struct ip_hdr *)(pkt + 14);
    tot = rd16((const u8 *)&ip->tot_len);
    if (tot > len - 14 || tot < 20)
        return;
    ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20)
        return;
    if (ip->dst != my_ip && ip->dst != 0xFFFFFFFFu)
        return; /* not for us */
    payload = (const u8 *)ip + ihl;
    switch (ip->proto) {
    case IP_ICMP:
        icmp_handle(ip, payload, tot - ihl);
        break;
    case IP_UDP:
        udp_handle(ip, payload, tot - ihl);
        break;
    case IP_TCP:
        tcp_handle(ip, payload, tot - ihl);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  TCP - single client connection                                     */
/* ------------------------------------------------------------------ */
#define TCP_CLOSED   0
#define TCP_SYN_SENT 1
#define TCP_ESTAB    2
#define TCP_FIN_WAIT 3
#define TCP_LISTEN   4
#define TCP_SYN_RCVD 5

#define TF_FIN 0x01
#define TF_SYN 0x02
#define TF_RST 0x04
#define TF_PSH 0x08
#define TF_ACK 0x10

static struct {
    int  state;
    u32  seq;        /* next seq to send */
    u32  acked;      /* last ack from peer */
    u32  rcv_nxt;    /* next seq expected */
    u32  peer_ip;
    u16  dport;
    u16  sport;
    u16  listen_port;
    u8  *rx;
    u32  rx_len;
    u32  rx_cap;
    int  rx_fin;
    int  error;
} tcp;

static void tcp_send_seg(u8 flags, u32 seq, const void *data, u16 dlen, u32 ack)
{
    u8 *p = tx_tcp;
    struct tcp_hdr *th = (struct tcp_hdr *)p;
    u16 seglen = 20 + dlen;

    memset(th, 0, 20);
    wr16((u8 *)&th->sport, tcp.sport);
    wr16((u8 *)&th->dport, tcp.dport);
    wr16((u8 *)&th->seq, (u16)(seq >> 16));
    wr16((u8 *)&th->seq + 2, (u16)seq);
    wr16((u8 *)&th->ack, (u16)(ack >> 16));
    wr16((u8 *)&th->ack + 2, (u16)ack);
    th->off_flags = 5 << 4;
    th->flags = flags;
    wr16((u8 *)&th->win, 0x4000);
    wr16((u8 *)&th->csum, 0);
    wr16((u8 *)&th->urg, 0);
    if (dlen)
        memcpy(p + 20, data, dlen);

    /* pseudo header checksum */
    {
        u8 ph[12];
        u32 sum = 0;
        memcpy(ph, &tcp.peer_ip, 4);
        memcpy(ph + 4, &my_ip, 4);
        ph[8] = 0;
        ph[9] = IP_TCP;
        wr16(ph + 10, seglen);
        sum = chksum16(ph, 12);
        sum += chksum16(p, seglen);
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        wr16((u8 *)&th->csum, (u16)~sum);
    }
    ip_send(tcp.peer_ip, IP_TCP, p, seglen);
}

void tcp_handle(const struct ip_hdr *ip, const u8 *payload, u16 plen)
{
    const struct tcp_hdr *th;
    u16 sport, dport;
    u32 seq, acknum;
    u8 flags;
    u16 hlen;
    const u8 *data;
    u16 dlen;

    if (tcp.state == TCP_CLOSED)
        return;
    if (plen < 20)
        return;
    th = (const struct tcp_hdr *)payload;
    sport = rd16((const u8 *)&th->sport);
    dport = rd16((const u8 *)&th->dport);
    if (dport != tcp.sport)
        return;
    if (tcp.state != TCP_LISTEN && (sport != tcp.dport || ip->src != tcp.peer_ip))
        return;

    seq = ((u32)rd16((const u8 *)&th->seq) << 16) | rd16((const u8 *)&th->seq + 2);
    acknum = ((u32)rd16((const u8 *)&th->ack) << 16) | rd16((const u8 *)&th->ack + 2);
    flags = th->flags;
    hlen = (th->off_flags >> 4) * 4;
    if (hlen < 20 || hlen > plen)
        return;
    data = payload + hlen;
    dlen = plen - hlen;

    if (flags & TF_RST) {
        tcp.error = 1;
        tcp.state = TCP_CLOSED;
        return;
    }

    if (tcp.state == TCP_LISTEN) {
        /* 新连接：SYN → 记录对端并回 SYN+ACK */
        if ((flags & TF_SYN) && !(flags & TF_ACK)) {
            tcp.peer_ip = ip->src;
            tcp.dport = sport;
            tcp.sport = tcp.listen_port;
            tcp.seq = (u32)(0x3000000 + (rdtsc() & 0xFFFFFF));
            tcp.acked = 0;
            tcp.rx_fin = 0;
            tcp.error = 0;
            tcp.rx_len = 0;
            tcp.rcv_nxt = seq + 1;
            tcp.state = TCP_SYN_RCVD;
            tcp_send_seg(TF_SYN | TF_ACK, tcp.seq, NULL, 0, tcp.rcv_nxt);
        }
        return;
    }
    if (tcp.state == TCP_SYN_RCVD) {
        /* 收到确认 → ESTAB */
        if ((flags & TF_ACK) && !(flags & TF_SYN)) {
            tcp.acked = acknum;
            tcp.state = TCP_ESTAB;
        }
        return;
    }
    if (tcp.state == TCP_SYN_SENT) {
        if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK)) {
            tcp.rcv_nxt = seq + 1;
            tcp.seq += 1;          /* account our SYN */
            tcp.acked = acknum;
            tcp.state = TCP_ESTAB;
            tcp_send_seg(TF_ACK, tcp.seq, NULL, 0, tcp.rcv_nxt);
        }
        return;
    }

    /* established / closing */
    if (flags & TF_ACK) {
        if (acknum > tcp.acked && acknum <= tcp.seq)
            tcp.acked = acknum;
    }
    if (dlen > 0) {
        if (seq != tcp.rcv_nxt) {
            /* out of order: re-ack expected */
            tcp_send_seg(TF_ACK, tcp.seq, NULL, 0, tcp.rcv_nxt);
            return;
        }
        if (tcp.rx_len + dlen <= tcp.rx_cap) {
            memcpy(tcp.rx + tcp.rx_len, data, dlen);
            tcp.rx_len += dlen;
        }
        tcp.rcv_nxt += dlen;
        tcp_send_seg(TF_ACK, tcp.seq, NULL, 0, tcp.rcv_nxt);
    }
    if (flags & TF_FIN) {
        tcp.rx_fin = 1;
        tcp.rcv_nxt += 1;
        tcp_send_seg(TF_ACK, tcp.seq, NULL, 0, tcp.rcv_nxt);
    }
}

int net_tcp_connect(u32 ip, u16 port)
{
    u32 t0;

    if (!up)
        return -1;
    if (tcp.state != TCP_CLOSED)
        return -1;

    tcp.state = TCP_SYN_SENT;
    tcp.peer_ip = ip;
    tcp.dport = port;
    tcp.sport = (u16)(40000 + (rdtsc() % 5000));
    tcp.seq = (u32)(0x1000000 + (rdtsc() & 0xFFFFFF));
    tcp.acked = 0;
    tcp.rcv_nxt = 0;
    tcp.rx_fin = 0;
    tcp.error = 0;
    tcp.rx_len = 0;
    if (!tcp.rx) {
        tcp.rx = (u8 *)kmalloc(16384);
        tcp.rx_cap = 16384;
    }

    tcp_send_seg(TF_SYN, tcp.seq, NULL, 0, 0);

    t0 = timer_uptime_ms();
    while (timer_uptime_ms() - t0 < 6000) {
        net_poll();
        if (tcp.state == TCP_ESTAB)
            return 0;
        if (tcp.error)
            return -1;
    }
    tcp.state = TCP_CLOSED;
    return -1;
}

int net_tcp_send(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u32 left = len;
    u32 t0;

    if (!up || tcp.state != TCP_ESTAB)
        return -1;

    while (left > 0) {
        u16 chunk = left > 1400 ? 1400 : (u16)left;
        u32 want_ack = tcp.seq + chunk;

        tcp_send_seg(TF_PSH | TF_ACK, tcp.seq, p, chunk, tcp.rcv_nxt);
        tcp.seq += chunk;
        p += chunk;
        left -= chunk;

        t0 = timer_uptime_ms();
        while (timer_uptime_ms() - t0 < 5000) {
            net_poll();
            if (tcp.error)
                return -1;
            if (tcp.acked >= want_ack)
                break;
        }
        if (tcp.acked < want_ack)
            return -1;
    }
    return (int)len;
}

int net_tcp_recv(void *buf, u32 len, u32 timeout_ms)
{
    u32 t0 = timer_uptime_ms();
    while (1) {
        if (tcp.rx_len > 0) {
            u32 n = tcp.rx_len < len ? tcp.rx_len : len;
            memcpy(buf, tcp.rx, n);
            memmove(tcp.rx, tcp.rx + n, tcp.rx_len - n);
            tcp.rx_len -= n;
            return (int)n;
        }
        if (tcp.rx_fin)
            return 0;
        if (tcp.error)
            return -1;
        net_poll();
        if (timer_uptime_ms() - t0 >= timeout_ms)
            return 0;
    }
}

int net_tcp_close(void)
{
    u32 t0;
    if (!up || tcp.state == TCP_CLOSED)
        return -1;
    if (tcp.state == TCP_ESTAB) {
        tcp_send_seg(TF_FIN | TF_ACK, tcp.seq, NULL, 0, tcp.rcv_nxt);
        tcp.state = TCP_FIN_WAIT;
        t0 = timer_uptime_ms();
        while (timer_uptime_ms() - t0 < 3000) {
            net_poll();
            if (tcp.rx_fin || tcp.error)
                break;
        }
    }
    tcp.state = TCP_CLOSED;
    return 0;
}

int net_tcp_listen(u16 port)
{
    if (!up)
        return -1;
    if (tcp.state != TCP_CLOSED)
        return -1;
    tcp.sport = port;
    tcp.listen_port = port;
    tcp.state = TCP_LISTEN;
    tcp.acked = 0;
    tcp.rcv_nxt = 0;
    tcp.rx_fin = 0;
    tcp.error = 0;
    tcp.rx_len = 0;
    return 0;
}

int net_tcp_accept(void)
{
    u32 t0;

    if (!up || tcp.state != TCP_LISTEN)
        return -1;
    t0 = timer_uptime_ms();
    while (timer_uptime_ms() - t0 < 15000) {
        net_poll();
        if (tcp.state == TCP_ESTAB)
            return 0;
        if (tcp.error)
            return -1;
    }
    tcp.state = TCP_LISTEN;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  DNS                                                               */
/* ------------------------------------------------------------------ */
static int dns_parse(u32 *ip_out)
{
    const u8 *p = dns_rx;
    u32 off = 12;
    u32 qd, an;
    int i;

    if (dns_rx_len < 12)
        return -1;
    qd = rd16(p + 4);
    an = rd16(p + 6);
    kprintf("[dbg] dns: len=%u qd=%u an=%u head=%02x%02x%02x%02x\n",
            dns_rx_len, qd, an, p[0], p[1], p[2], p[3]);
    for (i = 0; i < (int)qd; i++) {
        if (off + 1 > dns_rx_len)
            return -1;
        if (p[off] & 0xC0) {
            off += 2; /* compression pointer */
        } else {
            while (off < dns_rx_len && p[off] != 0)
                off += 1 + p[off];
            off += 1;
        }
        off += 4; /* type + class */
    }
    for (i = 0; i < (int)an; i++) {
        u16 type, rdlen;
        if (off + 1 > dns_rx_len)
            return -1;
        if (p[off] & 0xC0) {
            off += 2; /* pointer */
        } else {
            while (off < dns_rx_len && p[off] != 0)
                off += 1 + p[off];
            off += 1;
        }
        if (off + 10 > dns_rx_len)
            return -1;
        type  = rd16(p + off);      /* after name */
        kprintf("[dbg] dns an%d: off=%u type=%u\n", i, off, type);
        off += 2;                   /* type */
        off += 2;                   /* class */
        off += 4;                   /* ttl */
        if (off + 2 > dns_rx_len)
            return -1;
        rdlen = rd16(p + off);
        off += 2;                   /* rdlen */
        if (type == 1 && rdlen == 4 && off + 4 <= dns_rx_len) {
            *ip_out = (u32)p[off] | ((u32)p[off + 1] << 8) |
                      ((u32)p[off + 2] << 16) | ((u32)p[off + 3] << 24);
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

int net_dns_query(const char *name, u32 *ip_out)
{
    u8 q[512];
    u32 qlen = 12;
    u32 t0;
    const char *s = name;
    int attempt;

    if (!up)
        return -1;

    memset(q, 0, sizeof(q));
    dns_xid++;
    wr16(q + 0, dns_xid);
    wr16(q + 2, 0x0100); /* RD */
    wr16(q + 4, 1);      /* QDCOUNT */
    /* encode name */
    while (*s && qlen < 400) {
        const char *dot = s;
        u8 n = 0;
        while (*dot && *dot != '.') {
            dot++;
            n++;
        }
        q[qlen++] = n;
        memcpy(q + qlen, s, n);
        qlen += n;
        if (*dot == '.')
            s = dot + 1;
        else
            break;
    }
    q[qlen++] = 0;
    wr16(q + qlen, 1); /* QTYPE A */
    qlen += 2;
    wr16(q + qlen, 1); /* QCLASS IN */
    qlen += 2;

    for (attempt = 0; attempt < 3; attempt++) {
        u8 u[520]; /* UDP header + DNS payload */
        t0 = timer_uptime_ms();
        dns_done = 0;
        dns_rx_len = 0;
        wr16(u + 0, dns_sport);        /* sport */
        wr16(u + 2, 53);               /* dport */
        wr16(u + 4, (u16)(8 + qlen));  /* udp len */
        wr16(u + 6, 0);                /* csum */
        memcpy(u + 8, q, qlen);
        ip_send(my_dns, IP_UDP, u, (u16)(8 + qlen));
        while (timer_uptime_ms() - t0 < 2000) {
            net_poll();
            if (dns_done) {
                if (dns_parse(ip_out) == 0)
                    return 0;
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  entry points                                                       */
/* ------------------------------------------------------------------ */
static void net_rx(const u8 *buf, u16 len)
{
    u16 type;
    if (len < 14)
        return;
    type = rd16(buf + 12);
    if (type == ETH_ARP)
        arp_handle(buf, len);
    else if (type == ETH_IP)
        ip_handle(buf, len);
}

void net_poll(void)
{
    if (g_nic)
        g_nic->poll();
}

int net_up(void)
{
    return up;
}

int net_ping(u32 ip, int count, int timeout_ms, int *ok, u32 *avg_rtt_ms)
{
    int i, recv = 0;
    u32 total = 0;

    if (!up)
        return -1;
    if (ip == 0x7f000001) { /* 127.0.0.1 loopback */
        *ok = count;
        *avg_rtt_ms = 0;
        return count;
    }
    for (i = 0; i < count; i++) {
        u8 p[40];
        memset(p, 0, sizeof(p));
        p[0] = 8; /* echo request */
        p[1] = 0;
        wr16(p + 2, 0);
        wr16(p + 4, ping_id);
        wr16(p + 6, ++ping_seq);
        wr16(p + 2, chksum16(p, sizeof(p)));
        ping_want_seq = ping_seq;
        ping_done = 0;
        ping_t0 = timer_uptime_ms();
        ip_send(ip, IP_ICMP, p, sizeof(p));
        while (timer_uptime_ms() - ping_t0 < (u32)timeout_ms) {
            net_poll();
            if (ping_done)
                break;
        }
        if (ping_done) {
            recv++;
            total += ping_rtt;
        }
    }
    *ok = recv;
    *avg_rtt_ms = recv ? total / (u32)recv : 0;
    return recv;
}

void net_init(void)
{
    if (virtio_net_init(net_rx) == 0) {
        g_nic = &nic_virtio;
    } else if (rtl8139_init(net_rx) == 0) {
        g_nic = &nic_rtl8139;
    } else if (ne2000_init(net_rx) == 0) {
        g_nic = &nic_ne2000;
    } else if (e1000_init(net_rx) == 0) {
        g_nic = &nic_e1000;
    } else {
        kprintf("[net] no NIC driver matched\n");
        up = 0;
        return;
    }
    g_nic->mac(my_mac);
    up = 1;
    kprintf("[net] using NIC %s\n", g_nic->name);
    kprintf("[net] IP %d.%d.%d.%d gw %d.%d.%d.%d dns %d.%d.%d.%d\n",
            (u8)my_ip, (u8)(my_ip >> 8), (u8)(my_ip >> 16), (u8)(my_ip >> 24),
            (u8)my_gw, (u8)(my_gw >> 8), (u8)(my_gw >> 16), (u8)(my_gw >> 24),
            (u8)my_dns, (u8)(my_dns >> 8), (u8)(my_dns >> 16), (u8)(my_dns >> 24));
    arp_announce();
}

/* runtime IP override: real hardware / other subnets, avoids a reflash */
void net_set_ip(u32 ip, u32 gw, u32 mask)
{
    my_ip = ip;
    my_gw = gw;
    if (mask)
        my_mask = mask;
    my_dns = gw;
    if (g_nic)
        arp_announce();
    kprintf("[net] ip set to %d.%d.%d.%d gw %d.%d.%d.%d\n",
            (u8)my_ip, (u8)(my_ip >> 8), (u8)(my_ip >> 16), (u8)(my_ip >> 24),
            (u8)my_gw, (u8)(my_gw >> 8), (u8)(my_gw >> 16), (u8)(my_gw >> 24));
}

u32 net_get_ip(void) { return my_ip; }

u32 net_aton(const char *s)
{
    u32 r = 0;
    int i;
    for (i = 0; i < 4; i++) {
        u32 v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
        }
        r |= v << (8 * i);
        if (*s == '.')
            s++;
        else if (i < 3)
            return 0;
    }
    return r;
}

void net_ntoa(u32 ip, char *out)
{
    u32 o0 = ip & 0xFF, o1 = (ip >> 8) & 0xFF, o2 = (ip >> 16) & 0xFF, o3 = (ip >> 24) & 0xFF;
    char b[24];
    int n = 0, i, j;
    for (i = 0; i < 4; i++) {
        u32 v = (i == 0) ? o0 : (i == 1) ? o1 : (i == 2) ? o2 : o3;
        if (v >= 100) {
            b[n++] = '0' + v / 100;
            b[n++] = '0' + (v / 10) % 10;
        } else if (v >= 10) {
            b[n++] = '0' + v / 10;
        }
        b[n++] = '0' + v % 10;
        if (i < 3)
            b[n++] = '.';
    }
    for (j = 0; j < n; j++)
        out[j] = b[j];
    out[n] = 0;
}

void net_getinfo(netinfo_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = NET_MAGIC;
    out->ip = my_ip;
    out->gw = my_gw;
    out->dns = my_dns;
    out->mask = my_mask;
    memcpy(out->mac, my_mac, 6);
    out->up = up ? 1 : 0;
}
