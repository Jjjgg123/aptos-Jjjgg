#include "acpi.h"
#include "ata.h"
#include "blkdev.h"
#include "floppy.h"
#include "bootinfo.h"
#include "console.h"
#include "fs.h"
#include "gdt.h"
#include "gfx.h"
#include "gpu.h"
#include "gui.h"
#include "heap.h"
#include "idt.h"
#include "i18n.h"
#include "mem_ext.h"
#include "net.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "mouse.h"
#include "paging.h"
#include "pkg.h"
#include "kapi.h"

void term_say(const char *fmt, ...);
#include "syscall.h"
#include "scheduler.h"
#include "sysinfo.h"
#include "rtc.h"
#include "serial.h"
#include "timer.h"
#include "util.h"

/* ---- 硬件摘要：跨机器排查用（CPUID 不存在时安全跳过） ---- */
static int cpu_has_cpuid(void)
{
    u32 fl0, fl1;

    __asm__ volatile("pushfl\n\tpopl %0" : "=r"(fl0));
    fl1 = fl0 ^ (1u << 21);
    __asm__ volatile("pushl %0\n\tpopfl\n\tpushfl\n\tpopl %0" : "+r"(fl1));
    return ((fl0 ^ fl1) & (1u << 21)) != 0;
}

static void hw_cpu_report(void)
{
    u32 max, a, b, c, d;
    char vendor[13];

    if (!cpu_has_cpuid()) {
        kprintf("[cpu] CPUID not supported (386-class or BIOS-masked)\n");
        return;
    }

    __asm__ volatile("cpuid" : "=a"(max) : "a"(0) : "ebx", "ecx", "edx");
    if (max < 1) {
        kprintf("[cpu] cpuid max leaf 0 - model info unavailable\n");
        return;
    }

    __asm__ volatile("cpuid" : "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    vendor[0] = (char)(b & 0xFF);  vendor[1] = (char)((b >> 8) & 0xFF);
    vendor[2] = (char)((b >> 16) & 0xFF); vendor[3] = (char)((b >> 24) & 0xFF);
    vendor[4] = (char)(d & 0xFF);  vendor[5] = (char)((d >> 8) & 0xFF);
    vendor[6] = (char)((d >> 16) & 0xFF); vendor[7] = (char)((d >> 24) & 0xFF);
    vendor[8] = (char)(c & 0xFF);  vendor[9] = (char)((c >> 8) & 0xFF);
    vendor[10] = (char)((c >> 16) & 0xFF); vendor[11] = (char)((c >> 24) & 0xFF);
    vendor[12] = 0;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    {
        u32 fam = (a >> 8) & 0xF;
        u32 mod = (a >> 4) & 0xF;
        if (fam == 0xF)
            fam += (a >> 20) & 0xFF;
        if (fam == 0x6 || fam == 0xF)
            mod += ((a >> 16) & 0xF) << 4;
        kprintf("[cpu] %s family %d model %d stepping %d\n", vendor, fam, mod, a & 0xF);
        kprintf("[cpu] feat:%s%s%s%s%s%s\n",
                (d & (1u << 4)) ? " TSC" : "",
                (d & (1u << 8)) ? " CMOV" : "",
                (d & (1u << 9)) ? " APIC" : "",
                (d & (1u << 6)) ? " PAE" : "",
                (d & (1u << 25)) ? " SSE" : "",
                (d & (1u << 31)) ? " PBE" : "");
    }
}

void kmain(void)
{
    serial_init();
    klog_init();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  aptos 2.0 - 32-bit x86 kernel\n");
    kprintf("========================================\n");

    if (BOOTINFO->magic != BOOTINFO_MAGIC) {
        kprintf("[boot] FATAL: bad bootinfo magic 0x%x\n", BOOTINFO->magic);
        for (;;)
            hlt();
    }
    kprintf("[boot] framebuffer 0x%x %dx%d %dbpp, kernel lba %d, fs lba %d\n",
            BOOTINFO->fb_addr, BOOTINFO->fb_width, BOOTINFO->fb_height,
            BOOTINFO->fb_bpp, BOOTINFO->kernel_lba, BOOTINFO->fs_lba);

    heap_init();
    kprintf("[mem] usable RAM: %d MiB\n", mem_total_kb() / 1024);
    hw_cpu_report();
    mem_ext_report();
    kheap_init();
    kheap_selftest();

    gdt_init();
    idt_init();

    paging_init();

    gfx_init();
    gpu_init();
    console_init();
    i18n_init();

    sysinfo_init();

    timer_init(100);
    keyboard_init();
    mouse_init();
    acpi_init();
    syscall_init();
    scheduler_init();
    blk_init();
    ata_init();
    floppy_init();
    fs_init();
    net_init();

    sti();

    rtc_time_t t;
    rtc_read(&t);
    kprintf("[rtc] %04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day,
            t.hour, t.minute, t.second);
if (is_booting_from_hdd()) {
        /* 从系统盘加载 16x16 中文字体并应用安装时选择的语言 */
        extern void gfx_font16_set(const void *data);
        fs_dirent_t fe;
        u32 fsz = 0;
        if (fs_resolve("/FONT16.BIN", &fe) == 0)
            fsz = fe.size;
        if (fsz > 0 && fsz < 1024u * 1024u) {
            void *fbuf = kmalloc(fsz);
            if (fbuf && fs_read("/FONT16.BIN", fbuf, fsz) == (int)fsz) {
                gfx_font16_set(fbuf);
                kprintf("[gfx] CJK font loaded (%u bytes)\n", fsz);
            }
        }
        {
            fs_dirent_t le;
            char langbuf[16];
            if (fs_resolve("/LANG", &le) == 0 && le.size > 0 &&
                le.size < sizeof(langbuf)) {
                if (fs_read("/LANG", langbuf, le.size) == (int)le.size) {
                    langbuf[le.size] = 0;
                    for (int i = 0; langbuf[i]; i++)
                        if (langbuf[i] == '\n' || langbuf[i] == '\r')
                            langbuf[i] = 0;
                    if (i18n_set(langbuf) == 0)
                        kprintf("[i18n] language '%s' applied from /LANG\n", langbuf);
                }
            }
        }
    }
else {
        kprintf("[init] booted from CD-ROM (drive 0x%x) - looking for SETUP.LST\n",
                BOOTINFO->boot_drive);
        term_say("aptos setup disc detected\n");
        if (fs_mount("cd0", "/") == 0) {
            /* 从安装盘加载 16x16 中文字体，安装界面可显示中文 */
            extern void gfx_font16_set(const void *data);
            fs_dirent_t fe;
            u32 fsz = 0;
            if (fs_resolve("/FONT16.BIN", &fe) == 0)
                fsz = fe.size;
            if (fsz > 0 && fsz < 1024u * 1024u) {
                void *fbuf = kmalloc(fsz);
                if (fbuf && fs_read("/FONT16.BIN", fbuf, fsz) == (int)fsz) {
                    gfx_font16_set(fbuf);
                    kprintf("[gfx] CJK font loaded (%u bytes)\n", fsz);
                }
            }
            if (fs_exists("/SETUP.LST")) {
                int r = pkg_run("/SETUP.LST");
                kprintf("[init] setup result %d\n", r);
                term_say("setup exited with status %d\n", r);
            } else {
                term_say("SETUP.LST not found on the disc\n");
            }
        } else {
            term_say("cannot mount the CD-ROM\n");
        }
    }
    if (!gfx_available()) {
        kprintf("[init] WARNING: no usable VBE framebuffer - staying on the text console\n");
        console_update();
        for (;;)
            __asm__ volatile("hlt");
    }
        kprintf("[init] all subsystems up - starting the window manager\n");
    console_update();

    console_shutdown();
    gui_init();
    gui_open(&app_terminal);
    {
        extern void term_external_write(const char *buf, u32 len);
        syscall_set_output(term_external_write);
    }
    kprintf("[init] user mode ready: paging + ring 3 + int 0x80 syscalls\n");

#ifdef SCHED_AUTORUN
    {
        /* 验证用：输出直通串口 */
        extern void serial_putc(char c);
        void serial_out(const char *buf, u32 len) {
            for (u32 i = 0; i < len; i++) serial_putc(buf[i]);
        }
        syscall_set_output(serial_out);
        extern void term_autorun(void);
        term_autorun();
    }
#endif


    gui_run();
}
