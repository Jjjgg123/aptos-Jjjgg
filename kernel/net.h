#ifndef APTOS_NET_H
#define APTOS_NET_H

#include "types.h"

#define NET_MAGIC 0x54454E41u /* 'ANET' */

typedef struct {
    u32  magic;
    u32  ip;
    u32  gw;
    u32  dns;
    u32  mask;
    u8   mac[6];
    u8   up;
} netinfo_t;

void net_init(void);
void net_set_ip(u32 ip, u32 gw, u32 mask);
u32  net_get_ip(void);
void net_poll(void);
int  net_up(void);

/* IP address helpers */
u32  net_aton(const char *s);
void net_ntoa(u32 ip, char *out);

/* TCP client API (single connection) */
int  net_tcp_connect(u32 ip, u16 port);       /* blocking, returns 0/-1 */
int  net_tcp_send(const void *data, u32 len); /* blocking, returns bytes */
int  net_tcp_recv(void *buf, u32 len, u32 timeout_ms);
int  net_tcp_close(void);

/* ICMP ping: send count echo requests; returns replies received */
int  net_ping(u32 ip, int count, int timeout_ms, int *ok, u32 *avg_rtt_ms);

/* DNS: resolve host name -> IPv4 (blocking) */
int  net_dns_query(const char *name, u32 *ip_out);

void net_getinfo(netinfo_t *out);

#endif
