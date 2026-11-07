#ifndef APTOS_E1000_H
#define APTOS_E1000_H

#include "types.h"

/* packet receive callback: buffer + length */
typedef void (*e1000_rx_cb)(const u8 *buf, u16 len);

int  e1000_init(e1000_rx_cb cb);
int  e1000_send(const void *buf, u32 len);
void e1000_poll(void);
int  e1000_present(void);
void e1000_mac(u8 *mac_out);
u32  e1000_link_status(void);

#endif
