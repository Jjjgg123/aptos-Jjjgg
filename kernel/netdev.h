#ifndef APTOS_NETDEV_H
#define APTOS_NETDEV_H

#include "types.h"

/* packet receive callback (shared with all NIC drivers) */
typedef void (*nic_rx_cb)(const u8 *buf, u16 len);

typedef struct nic_ops {
    int  (*init)(nic_rx_cb cb);
    int  (*send)(const void *buf, u32 len);
    void (*poll)(void);
    int  (*present)(void);
    void (*mac)(u8 *mac_out);
    const char *name;
} nic_ops_t;

/* driver instances (implemented in e1000.c / rtl8139.c / ne2000.c) */
extern nic_ops_t nic_e1000;
extern nic_ops_t nic_rtl8139;
extern nic_ops_t nic_ne2000;
extern nic_ops_t nic_virtio;

/* driver init entry points (called by net_init probing) */
int rtl8139_init(nic_rx_cb cb);
int ne2000_init(nic_rx_cb cb);
int virtio_net_init(nic_rx_cb cb);

/* currently selected NIC (set by net_init, used by net.c) */
extern nic_ops_t *g_nic;

#endif
