#ifndef APTOS_PCI_H
#define APTOS_PCI_H

#include "types.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

u32 pci_read(u8 bus, u8 dev, u8 fn, u8 reg);
void pci_write(u8 bus, u8 dev, u8 fn, u8 reg, u32 val);
/* returns 1 if a device with vendor/device was found; fills bus/dev/fn/bar0 */
int  pci_find(u16 vendor, u32 device_mask, u16 device_want,
              u8 *bus, u8 *dev, u8 *fn, u32 *bar0);
/* 返回某个 BAR 的窗口大小（字节，2 的幂）；0 表示不存在/无法探测。
 * 内核内部完成“写全 1 -> 读回 -> 恢复”的原子操作，用户态不需要直接写配置空间。 */
u32  pci_bar_size(u8 bus, u8 dev, u8 fn, u8 baridx);

/* enable I/O+memory access and bus mastering */
void pci_enable_master(u8 bus, u8 dev, u8 fn);

#endif
