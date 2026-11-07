/* 显示设备探测与加速能力评估。
 *
 * 设计原则：不绑死某一种虚拟化平台或某一块显卡。PCI 扫描、BAR 解析、
 * 寄存器寻址方式全部靠探测得出；探测失败就明确报告"本机只能用帧缓冲"，
 * 由 gfx.c 继续走 VBE 线性帧缓冲，不会因为拿不到加速就黑屏。
 *
 * 目前实现的是 VMware SVGA II（PCI 15ad:0405）的探测部分。真机上如果
 * 遇到同类设备可以直接复用；遇到别的显卡则报告为不支持，不影响启动。
 */

#include "gpu.h"
#include "io.h"
#include "klog.h"
#include "pci.h"

/* ---- SVGA 寄存器编号（VMware SVGA II 规范） ---- */
#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_MAX_WIDTH      4
#define SVGA_REG_MAX_HEIGHT     5
#define SVGA_REG_BPP            7
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_VRAM_SIZE      15
#define SVGA_REG_FB_SIZE        16
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_MEM_START      18
#define SVGA_REG_MEM_SIZE       19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22

#define SVGA_ID_2               0x90000002u

typedef struct {
    u8  bus, dev, fn;
    u16 vendor, device;
    u32 bar0, bar2;
    int found;
} display_dev_t;

static display_dev_t disp;

/* 扫描 PCI 找显示类设备（class 0x03）。只报告，不假设总线编号。 */
static void scan_display(void)
{
    u8 b, d, f;

    for (b = 0; b < 4; b++) {
        for (d = 0; d < 32; d++) {
            for (f = 0; f < 8; f++) {
                u32 id = pci_read(b, d, f, 0x00);
                u32 cls;
                u16 vendor, device;

                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0x0000)
                    continue;

                vendor = (u16)(id & 0xFFFF);
                device = (u16)(id >> 16);
                cls = pci_read(b, d, f, 0x08);

                kprintf("[pci] %02x:%02x.%x %04x:%04x class=%02x sub=%02x\n",
                        b, d, f, vendor, device,
                        (u32)(cls >> 24) & 0xFF, (u32)(cls >> 16) & 0xFF);

                if (((cls >> 24) & 0xFF) == 0x03 && !disp.found) {
                    disp.found = 1;
                    disp.bus = b;
                    disp.dev = d;
                    disp.fn = f;
                    disp.vendor = vendor;
                    disp.device = device;
                    /* 打开 I/O 与显存空间访问，否则端口读回全 0xFFFFFFFF */
                    pci_enable_master(b, d, f);
                    disp.bar0 = pci_read(b, d, f, 0x10) & ~0x3u;
                    disp.bar2 = pci_read(b, d, f, 0x18) & ~0xFu;
                }

                if (f == 0 && ((pci_read(b, d, 0, 0x0C) >> 16) & 0x80) == 0)
                    break;
            }
        }
    }
}

/* 探测 SVGA 的 IO 寄存器布局：索引端口在 +0，数据端口可能在 +4/+8/+12。
   写 ID 再读回，哪个偏移能对上就用哪个；一个都对不上就判定不可用。 */
static int svga_detect_ports(u32 iobase)
{
    int off;

    for (off = 4; off < 16; off += 4) {
        outl(iobase + 0, SVGA_REG_ID);
        outl(iobase + off, SVGA_ID_2);
        outl(iobase + 0, SVGA_REG_ID);
        if (inl(iobase + off) == SVGA_ID_2)
            return off;
    }
    return -1;
}

/* 评估 SVGA 是否可用，返回 0 表示可以走加速路径 */
static int svga_probe(u32 iobase)
{
    int voff;

    voff = svga_detect_ports(iobase);
    if (voff < 0) {
        kprintf("[svga] io bar has no usable data port (index=%x)\n", iobase);
        return -1;
    }

    kprintf("[svga] index +0, data +%x\n", voff);

    {
        u32 maxw, maxh, vram, enable, cfg, fb, mem;

        outl(iobase + 0, SVGA_REG_ID);
        outl(iobase + voff, SVGA_ID_2);

        outl(iobase + 0, SVGA_REG_MAX_WIDTH);
        maxw = inl(iobase + voff);
        outl(iobase + 0, SVGA_REG_MAX_HEIGHT);
        maxh = inl(iobase + voff);
        outl(iobase + 0, SVGA_REG_VRAM_SIZE);
        vram = inl(iobase + voff);
        outl(iobase + 0, SVGA_REG_ENABLE);
        enable = inl(iobase + voff);
        outl(iobase + 0, SVGA_REG_CONFIG_DONE);
        cfg = inl(iobase + voff);
        outl(iobase + 0, SVGA_REG_FB_START);
        fb = inl(iobase + voff);
        outl(iobase + 0, SVGA_REG_MEM_START);
        mem = inl(iobase + voff);

        kprintf("[svga] max=%ux%u vram=%u enable=%u cfg=%u fb=%x mem=%x\n",
                maxw, maxh, vram, enable, cfg, fb, mem);

        /* 寄存器读回 0xFFFFFFFF 说明后端没有真正实现这套接口 */
        if (maxw == 0xFFFFFFFFu || vram == 0xFFFFFFFFu) {
            kprintf("[svga] registers not implemented by backend, "
                    "framebuffer path only\n");
            return -1;
        }
    }

    return 0;
}

void gpu_init(void)
{
    scan_display();

    if (!disp.found) {
        kprintf("[gpu] no display-class device, using VBE framebuffer\n");
        return;
    }

    kprintf("[gpu] display %04x:%04x at %02x:%02x.%x bar0=%x bar2=%x\n",
            disp.vendor, disp.device, disp.bus, disp.dev, disp.fn,
            disp.bar0, disp.bar2);

    if (disp.vendor == 0x15AD) {
        if (svga_probe(disp.bar0) == 0) {
            kprintf("[gpu] acceleration backend: VMware SVGA II\n");
            return;
        }
        kprintf("[gpu] acceleration backend: unavailable\n");
        return;
    }

    if (disp.vendor == 0x80EE) {
        kprintf("[gpu] VirtualBox VGA: VBE only, no 2D engine\n");
        return;
    }

    kprintf("[gpu] unknown vendor, using VBE framebuffer\n");
}
