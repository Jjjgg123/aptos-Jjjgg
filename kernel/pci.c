#include "pci.h"
#include "io.h"
#include "klog.h"

u32 pci_read(u8 bus, u8 dev, u8 fn, u8 reg)
{
    outl(PCI_CONFIG_ADDR, 0x80000000u | ((u32)bus << 16) |
                          ((u32)dev << 11) | ((u32)fn << 8) | (reg & 0xFCu));
    return inl(PCI_CONFIG_DATA);
}

void pci_write(u8 bus, u8 dev, u8 fn, u8 reg, u32 val)
{
    outl(PCI_CONFIG_ADDR, 0x80000000u | ((u32)bus << 16) |
                          ((u32)dev << 11) | ((u32)fn << 8) | (reg & 0xFCu));
    outl(PCI_CONFIG_DATA, val);
}

int pci_find(u16 vendor, u32 device_mask, u16 device_want,
             u8 *bus, u8 *dev, u8 *fn, u32 *bar0)
{
    u8 b, d;
    for (b = 0; b < 1; b++) {
        for (d = 0; d < 32; d++) {
            u32 id = pci_read(b, d, 0, 0);
            u16 v = id & 0xFFFF;
            u16 dev_id = id >> 16;
            if (v == 0xFFFF || v == 0x0000)
                continue;
            if (v == vendor && (dev_id & device_mask) == (device_want & device_mask)) {
                *bus = b;
                *dev = d;
                *fn = 0;
                *bar0 = pci_read(b, d, 0, 0x10);
                return 1;
            }
        }
    }
    return 0;
}

u32 pci_bar_size(u8 bus, u8 dev, u8 fn, u8 baridx)
{
    u8 reg;
    u32 orig, val, size, cmd, mask;

    if (baridx > 5)
        return 0;

    reg = (u8)(0x10 + baridx * 4);
    orig = pci_read(bus, dev, fn, reg);
    if (orig == 0u || orig == 0xFFFFFFFFu)
        return 0;

    /* 探测期间关掉 I/O + 内存解码，避免设备真的去响应全 1 地址 */
    cmd = pci_read(bus, dev, fn, 0x04);
    pci_write(bus, dev, fn, 0x04, cmd & ~0x3u);

    pci_write(bus, dev, fn, reg, 0xFFFFFFFFu);
    val = pci_read(bus, dev, fn, reg);

    pci_write(bus, dev, fn, reg, orig);
    pci_write(bus, dev, fn, 0x04, cmd);

    if (val == 0u || val == 0xFFFFFFFFu)
        return 0;

    /* bit0=1 是 I/O 空间，低 2 位是标志；否则是 MMIO，低 4 位是标志 */
    mask = (orig & 0x1u) ? 0xFFFFFFFCu : 0xFFFFFFF0u;
    size = (~(val & mask)) + 1u;

    if (size == 0u || (size & (size - 1u)) != 0u)
        return 0;                    /* 不是 2 的幂，说明探测不可信 */
    return size;
}

void pci_enable_master(u8 bus, u8 dev, u8 fn)
{
    u32 cmd = pci_read(bus, dev, fn, 0x04);
    cmd |= 0x0007; /* I/O space, memory space, bus master */
    pci_write(bus, dev, fn, 0x04, cmd);
}
