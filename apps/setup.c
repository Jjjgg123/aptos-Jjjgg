#include <stdarg.h>
#include "aptos.h"

#define ERT_MAX (512 * 1024)
#define BLOB_MAX (128 * 1024)
#define FILE_MAX (320 * 1024)

#define ERT_HEADER 24
#define MIN_MATCH 3

#define WIN_W 760
#define WIN_H 520
#define HEADER_H 52
#define FOOTER_H 56
#define BTN_W 104
#define BTN_H 30
#define BTN_Y (WIN_H - BTN_H - 14)

#define C_HEAD   RGB(22, 54, 100)
#define C_HEAD2  RGB(36, 82, 142)
#define C_BODY   RGB(255, 255, 255)
#define C_FRAME  RGB(158, 168, 182)
#define C_TEXT   RGB(38, 44, 56)
#define C_DIM    RGB(118, 128, 144)
#define C_GREEN  RGB(38, 132, 62)
#define C_BLUE   RGB(38, 108, 178)
#define C_RED    RGB(190, 40, 40)
#define C_FOOT   RGB(238, 240, 245)
#define C_BTN    RGB(228, 232, 240)
#define C_BTNHOT RGB(206, 220, 240)

#define PAGE_WELCOME 0
#define PAGE_TARGET  1
#define PAGE_INSTALL 2
#define PAGE_DONE    3

/* ---- 语言支持：setup 自带 en/zh 语言包（可扩展） ---- */
static int lang_cur;  /* 0=en 1=zh */
static int zh_pkg;    /* 是否把中文语言包写入目标盘（英文默认安装） */

static const char *const ST[][3] = {
    /* key                            en                                    zh */
    { "hdr.welcome",  "Welcome to the aptos 2.0 Setup Wizard",             "欢迎使用 aptos 2.0 安装向导" },
    { "hdr.target",   "Select the target disk",                            "选择目标磁盘" },
    { "hdr.targetsub","Setup lists the hard disks it found",               "安装程序列出它找到的硬盘" },
    { "hdr.install",  "Installing aptos 2.0",                              "正在安装 aptos 2.0" },
    { "hdr.installsub","This may take a moment",                           "这可能需要一点时间" },
    { "hdr.done",     "Completing the aptos 2.0 Setup Wizard",             "正在完成 aptos 2.0 安装向导" },
    { "w.body",       "This wizard will install aptos 2.0 on your computer.",
                      "此向导将在您的计算机上安装 aptos 2.0。" },
    { "w.setupwill",  "Setup will:",                                        "安装程序将：" },
    { "w.s1",         "- write the boot loader and the kernel to the hard disk",
                      "- 把引导程序与内核写入硬盘" },
    { "w.s2",         "- create a fresh MINIFS02 or FAT filesystem on the selected disk",
                      "- 在所选磁盘上创建全新的 MINIFS02 或 FAT 文件系统" },
    { "w.s3",         "- install the system files, the shell and the applications",
                      "- 安装系统文件、外壳与应用程序" },
    { "w.s4",         "- put every application on the desktop, ready to run",
                      "- 把所有应用程序放到桌面，随时可运行" },
    { "w.cont",       "To continue, click Next. To quit setup, click Cancel.",
                      "要继续，请点击“下一步”。要退出安装，请点击“取消”。" },
    { "w.live1",      "This disc is also a live system: if you boot it with a hard",
                      "这张光盘也是一个可运行系统：如果带着硬盘启动它，" },
    { "w.live2",      "disk attached you can install, if you boot setup from an",
                      "你就可以安装；如果从已安装的磁盘启动安装程序，" },
    { "w.live3",      "installed disk you will get what you deserve.",
                      "你会得到你应得的。" },
    { "w.language",   "Language / 语言",                                     "语言 / Language" },
    { "t.disk",       "Disk",                                               "磁盘" },
    { "t.size",       "Size",                                               "容量" },
    { "t.type",       "Type",                                               "类型" },
    { "t.nodisk",     "No writable hard disk found. Attach one and restart setup.",
                      "未找到可写的硬盘。请接上一块硬盘并重新启动安装程序。" },
    { "t.ide",        "IDE hard disk",                                      "IDE 硬盘" },
    { "t.toosmall",   "too small",                                          "容量不足" },
    { "t.selected",   "selected",                                           "已选中" },
    { "t.need",       "aptos needs a disk of at least %d KiB (system image %d KiB).",
                      "aptos 至少需要 %d KiB 的磁盘（系统映像 %d KiB）。" },
    { "t.fs",         "Filesystem",                                         "文件系统" },
    { "t.smallwarn",  "The selected disk is too small. Pick another one.",
                      "所选磁盘容量不足，请另选一块。" },
    { "t.erase",      "Warning: everything on the selected disk will be erased.",
                      "警告：所选磁盘上的所有数据都将被擦除。" },
    { "t.fresh",      "The disk gets a fresh %s filesystem, the boot loader goes to",
                      "该磁盘将获得全新的 %s 文件系统，引导程序写入" },
    { "t.sector0",    "sector 0, and the system snapshot is unpacked onto it.",
                      "第 0 扇区，系统快照将被解压到其上。" },
    { "t.zhpkg",      "Install the Chinese language pack (English is default, press Z)",
                      "安装中文语言包（英文默认安装，按 Z 切换）" },
    { "t.zhpkg.on",   "[X] install ZH language pack",                       "[X] 安装中文语言包" },
    { "t.zhpkg.off",  "[ ] install ZH language pack",                       "[ ] 安装中文语言包" },
    { "i.status",     "Setup is copying files, please wait.",
                      "安装程序正在复制文件，请稍候。" },
    { "d.done",       "Setup completed successfully.",                      "安装成功完成。" },
    { "d.ondisk",     "aptos 2.0 is now on your hard disk. Remove the CD-ROM and",
                      "aptos 2.0 现在已安装到您的硬盘。取出光盘并" },
    { "d.reboot",     "reboot: the machine will start from the hard disk and show",
                      "重新启动：机器将从硬盘启动并显示" },
    { "d.desktop",    "the desktop with all applications ready to run.",
                      "桌面，所有应用程序均可运行。" },
    { "d.finish",     "Click Finish to reboot now.",                        "点击“完成”立即重新启动。" },
    { "b.cancel",     "Cancel",                                             "取消" },
    { "b.next",       "Next >",                                             "下一步 >" },
    { "b.back",       "< Back",                                             "< 上一步" },
    { "b.install",    "Install",                                            "安装" },
    { "b.finish",     "Finish",                                             "完成" },
    { "b.working",    "Working...",                                         "正在处理…" },
    { "e.cancel",     "Setup cancelled.",                                   "安装已取消。" },
    { "e.open",       "cannot open /SYSTEM.ERT (error %d)",                 "无法打开 /SYSTEM.ERT（错误 %d）" },
    { "e.badert",     "SYSTEM.ERT is not a valid snapshot",                 "SYSTEM.ERT 不是有效的系统快照" },
    { "e.damaged",    "the system snapshot looks damaged",                  "系统快照已损坏" },
    { "e.prep",       "Preparing to install...",                            "正在准备安装…" },
    { "e.read",       "Snapshot read: %d bytes, %d entries",                "已读取系统快照：%d 字节，%d 项" },
    { "e.unpack",     "Boot image unpacked (%d bytes)",                     "引导映像已解包（%d 字节）" },
    { "e.bootwrite",  "Boot loader and kernel written to sector 0",         "引导程序与内核已写入第 0 扇区" },
    { "e.baddecomp",  "boot image decompression failed (%d of %d)",         "引导映像解压失败（%d / %d）" },
    { "e.writeraw",   "cannot write the boot image to %s (error %d)",       "无法把引导映像写入 %s（错误 %d）" },
    { "e.toosmall2",  "%s is too small: the system needs %d KiB, the disk has %d KiB",
                      "%s 容量不足：系统需要 %d KiB，磁盘只有 %d KiB" },
    { "e.mkfs",       "cannot create the filesystem on %s (error %d)",      "无法在 %s 上创建文件系统（错误 %d）" },
    { "e.created",    "%s filesystem created at sector %d",                 "%s 文件系统已创建于第 %d 扇区" },
    { "e.trunc",      "the snapshot is truncated",                          "系统快照被截断" },
    { "e.badentry",   "bad entry #%d at pos %d: nlen %d raw %d comp %d, ert %d",
                      "错误条目 #%d 位于 %d：nlen %d raw %d comp %d, ert %d" },
    { "e.big",        "%s is too big for this build, skipped",              "%s 太大，此构建已跳过" },
    { "e.decomp",     "%s: decompression failed",                           "%s：解压失败" },
    { "e.create",     "%s: cannot create (error %d)",                       "%s：无法创建（错误 %d）" },
    { "e.write",      "%s: write failed",                                   "%s：写入失败" },
    { "e.flush",      "%s: could not flush to the disk",                    "%s：无法刷新到磁盘" },
    { "e.copy",       "Installing %s  (%d bytes)",                          "正在安装 %s（%d 字节）" },
    { "e.mkdir",      "Creating folder %s",                                 "正在创建文件夹 %s" },
    { "win.title",    "aptos 2.0 Setup",                                    "aptos 2.0 安装" },
};

static const char *t(const char *key)
{
    int n = (int)(sizeof(ST) / sizeof(ST[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(key, ST[i][0]) == 0)
            return ST[i][lang_cur + 1];
    }
    return key;
}

/* UTF-8 显示宽度：ASCII 8px，汉字 16px */
static int utf8_w(const char *s)
{
    int w = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 0x80) {
            w += 8;
        } else if (c >= 0xC0 && c < 0xE0) {
            w += 8;
            s++;
        } else if (c >= 0xE0) {
            w += 16;
            s += 2;
        }
    }
    return w;
}

static unsigned char ert[ERT_MAX];
static unsigned char blob[BLOB_MAX];
static unsigned char filebuf[FILE_MAX];

static int have_win;
static int page;
static int ert_len;
static int entry_count;
static int blob_len;
static int blob_c;
static int fs_lba;
static int pos;
static int step;
static int total_steps;
static int done_steps;
static int cur_dev;
static int disk_count;
static int min_sectors;
static int disk_fits(int i);
static char disk_name[8][16];
static int  disk_sec[8];
static char status[128];
static int  failed;
static int  fs_choice;
static const char *fs_names[4] = { "MINIFS02", "FAT12", "FAT16", "FAT32" };

static int prev_btn;
static int last_mx = -1, last_my = -1;

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static int lzss_decompress(const unsigned char *src, int slen, unsigned char *dst,
                           int dlen)
{
    int si = 0, di = 0, bits = 0;
    unsigned char flags = 0;

    while (si < slen && di < dlen) {
        if (bits == 0) {
            if (si >= slen)
                break;
            flags = src[si++];
            bits = 8;
        }
        if (flags & 1) {
            if (si >= slen)
                break;
            dst[di++] = src[si++];
        } else {
            int b0, b1, off, len, p;
            if (si + 1 >= slen)
                break;
            b0 = src[si];
            b1 = src[si + 1];
            si += 2;
            off = b0 | ((b1 & 0x0F) << 8);
            len = (b1 >> 4) + MIN_MATCH;
            p = di - off;
            if (p < 0)
                return -1;
            while (len-- > 0 && di < dlen)
                dst[di++] = dst[p++];
        }
        flags >>= 1;
        bits--;
    }
    return di;
}

static void text(int x, int y, const char *s, int color, int scale)
{
    if (have_win)
        sys_text(x, y, s, color, scale);
}

static void box(int x, int y, int w, int h, int fill, int border)
{
    if (!have_win)
        return;
    if (fill >= 0)
        sys_rect(x, y, w, h, fill);
    if (border >= 0)
        sys_frame(x, y, w, h, border);
}

static void button(int id, const char *label, int x, int y, int enabled)
{
    int mx = 0, my = 0, mb = 0, inside = 0;
    int hot = 0;
    int tw;

    sys_mouse(&mx, &my, &mb, &inside);
    if (inside && mx >= x && my >= y && mx < x + BTN_W && my < y + BTN_H)
        hot = 1;

    box(x, y, BTN_W, BTN_H, (hot && enabled) ? C_BTNHOT : C_BTN, C_FRAME);
    if (enabled)
        sys_rect(x + 1, y + 1, BTN_W - 2, 1, RGB(255, 255, 255));
    tw = utf8_w(label);
    text(x + (BTN_W - tw) / 2, y + (BTN_H - 8) / 2, label,
         enabled ? C_TEXT : C_DIM, 1);
    (void)id;
}

static void header(const char *title, const char *sub)
{
    box(0, 0, WIN_W, HEADER_H, C_HEAD, -1);
    sys_rect(0, HEADER_H - 6, WIN_W, 6, C_HEAD2);
    text(20, 10, title, RGB(255, 255, 255), 2);
    if (sub)
        text(20, 34, sub, RGB(186, 206, 232), 1);
}

static void progress_bar(int percent)
{
    int x = 60, y = 232, w = WIN_W - 120, h = 22;
    int fill = (w - 4) * percent / 100;
    char buf[16];

    box(x, y, w, h, RGB(246, 248, 250), C_FRAME);
    if (fill > 0)
        sys_rect(x + 2, y + 2, fill, h - 4, C_GREEN);
    for (int i = 1; i < 10; i++)
        sys_rect(x + i * (w / 10), y + 2, 1, h - 4, RGB(220, 226, 234));
    snprintf(buf, sizeof(buf), "%d%%", percent);
    text(WIN_W / 2 - (int)strlen(buf) * 4, y + h + 10, buf, C_DIM, 1);
}

static void checkmark(int cx, int cy)
{
    for (int i = 0; i < 24; i++)
        sys_rect(cx - 34 + i, cy + i, 6, 6, C_GREEN);
    for (int i = 0; i < 54; i++)
        sys_rect(cx - 10 + i, cy + 24 - i, 6, 6, C_GREEN);
}

static void disk_scan(void)
{
    int i, n = 0;

    for (i = 0; i < 8 && n < 8; i++) {
        char nm[16];
        int sec = 0, type = 0;
        if (sys_disk_info(i, nm, &sec, &type) != 1)
            continue;
        if (type != 1)
            continue;
        for (int k = 0; k < 16; k++)
            disk_name[n][k] = nm[k];
        disk_sec[n] = sec;
        n++;
    }
    disk_count = n;
}

static void draw_page(void)
{
    if (!have_win)
        return;
    sys_clear(C_BODY);

    if (page == PAGE_WELCOME) {
        header(t("hdr.welcome"), NULL);
        /* 语言选择 */
        text(60, 66, t("w.language"), C_TEXT, 1);
        for (int i = 0; i < 2; i++) {
            int x = 60 + i * 110;
            int sel = (i == lang_cur);
            const char *lab = (i == 0) ? "English" : "简体中文";
            int tw = utf8_w(lab);
            box(x, 80, 102, 26, sel ? RGB(220, 234, 250) : RGB(248, 250, 252),
                sel ? C_BLUE : C_FRAME);
            text(x + (102 - tw) / 2, 89, lab, sel ? C_HEAD : C_TEXT, 1);
        }
        text(60, 92 + 30, t("w.body"), C_TEXT, 1);
        text(60, 124 + 30, t("w.setupwill"), C_TEXT, 1);
        text(78, 146 + 30, t("w.s1"), C_DIM, 1);
        text(78, 164 + 30, t("w.s2"), C_DIM, 1);
        text(78, 182 + 30, t("w.s3"), C_DIM, 1);
        text(78, 200 + 30, t("w.s4"), C_DIM, 1);
        text(60, 240 + 30, t("w.cont"), C_TEXT, 1);
        text(60, 286 + 30, t("w.live1"), C_DIM, 1);
        text(60, 304 + 30, t("w.live2"), C_DIM, 1);
        text(60, 322 + 30, t("w.live3"), C_DIM, 1);
    } else if (page == PAGE_TARGET) {
        header(t("hdr.target"), t("hdr.targetsub"));
        text(60, 84, t("t.disk"), C_TEXT, 1);
        text(220, 84, t("t.size"), C_TEXT, 1);
        text(360, 84, t("t.type"), C_TEXT, 1);
        if (disk_count == 0) {
            text(60, 118, t("t.nodisk"), C_RED, 1);
        }
        for (int i = 0; i < disk_count; i++) {
            int y = 106 + i * 34;
            int sel = (i == cur_dev);
            int fits = disk_fits(i);
            char buf[48];
            box(52, y, WIN_W - 104, 30, sel ? RGB(220, 234, 250) : RGB(248, 250, 252),
                sel ? C_BLUE : C_FRAME);
            text(64, y + 11, disk_name[i], sel ? C_HEAD : C_TEXT, 1);
            snprintf(buf, sizeof(buf), "%d MiB", disk_sec[i] / 2048);
            text(220, y + 11, buf, C_TEXT, 1);
            text(360, y + 11, t("t.ide"), C_DIM, 1);
            if (!fits)
                text(WIN_W - 156, y + 11, t("t.toosmall"), C_RED, 1);
            else if (sel)
                text(WIN_W - 116, y + 11, t("t.selected"), C_GREEN, 1);
        }
        {
            char buf[128];
            int fty = 106 + disk_count * 34;
            snprintf(buf, sizeof(buf), t("t.need"),
                     min_sectors / 2, blob_len / 1024);
            text(60, WIN_H - 168, buf, C_DIM, 1);
            text(60, fty + 6, t("t.fs"), C_TEXT, 1);
            for (int i = 0; i < 4; i++) {
                int x = 60 + i * 132;
                int y = fty + 24;
                int sel = (i == fs_choice);
                box(x, y, 124, 26, sel ? RGB(220, 234, 250) : RGB(248, 250, 252),
                    sel ? C_BLUE : C_FRAME);
                text(x + 10, y + 9, fs_names[i], sel ? C_HEAD : C_TEXT, 1);
            }
        }
        if (disk_count > 0 && !disk_fits(cur_dev))
            text(60, WIN_H - 150, t("t.smallwarn"), C_RED, 1);
        else
            text(60, WIN_H - 150, t("t.erase"), C_RED, 1);
        {
            char buf[160];
            snprintf(buf, sizeof(buf), t("t.fresh"), fs_names[fs_choice]);
            text(60, WIN_H - 126, buf, C_DIM, 1);
        }
        text(60, WIN_H - 108, t("t.sector0"), C_DIM, 1);
        text(60, WIN_H - 86, t("t.zhpkg"), C_TEXT, 1);
        text(60, WIN_H - 64, zh_pkg ? t("t.zhpkg.on") : t("t.zhpkg.off"),
             zh_pkg ? C_GREEN : C_DIM, 1);
    } else if (page == PAGE_INSTALL) {
        int percent = total_steps ? (done_steps * 100 / total_steps) : 0;
        header(t("hdr.install"), t("hdr.installsub"));
        progress_bar(percent);
        text(60, 300, status, failed ? C_RED : C_TEXT, 1);
        if (!failed)
            text(60, 326, t("i.status"), C_DIM, 1);
    } else {
        header(t("hdr.done"), NULL);
        checkmark(WIN_W / 2 - 46, 116);
        text(WIN_W / 2 - 112, 206, t("d.done"), C_GREEN, 2);
        text(60, 256, t("d.ondisk"), C_TEXT, 1);
        text(60, 276, t("d.reboot"), C_TEXT, 1);
        text(60, 296, t("d.desktop"), C_TEXT, 1);
        text(60, 336, t("d.finish"), C_DIM, 1);
    }

    box(0, WIN_H - FOOTER_H, WIN_W, FOOTER_H, C_FOOT, -1);
    sys_rect(0, WIN_H - FOOTER_H, WIN_W, 1, C_FRAME);

    if (page == PAGE_WELCOME) {
        button(0, t("b.cancel"), WIN_W - 2 * (BTN_W + 10) - 18, BTN_Y, 1);
        button(1, t("b.next"), WIN_W - (BTN_W + 10) - 18, BTN_Y, 1);
    } else if (page == PAGE_TARGET) {
        button(2, t("b.back"), WIN_W - 3 * (BTN_W + 10) - 18, BTN_Y, 1);
        button(3, t("b.install"), WIN_W - (BTN_W + 10) - 18, BTN_Y,
               disk_count > 0 && disk_fits(cur_dev));
    } else if (page == PAGE_DONE) {
        button(4, t("b.finish"), WIN_W - (BTN_W + 10) - 18, BTN_Y, 1);
    } else {
        button(5, t("b.working"), WIN_W - (BTN_W + 10) - 18, BTN_Y, 0);
    }
    sys_flush();
}

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    if (!have_win)
        printf("%s\n", status);
}

static int load_snapshot(void)
{
    int fd, n, total = 0;

    fd = sys_open("/SYSTEM.ERT", 0);
    if (fd < 0) {
        say(t("e.open"), fd);
        return -1;
    }
    while (total < ERT_MAX) {
        n = sys_read(fd, ert + total, 32768);
        if (n <= 0)
            break;
        total += n;
    }
    sys_close(fd);
    if (total < ERT_HEADER || ert[0] != 'E' || ert[1] != 'R' || ert[2] != 'T' ||
        ert[3] != '1') {
        say(t("e.badert"));
        return -1;
    }
    return total;
}

static int snapshot_ok;

#define FS_MIN_SECTORS 2048

static int read_snapshot_header(void)
{
    ert_len = load_snapshot();
    if (ert_len <= 0)
        return -1;
    blob_len = (int)rd32(ert + 8);
    blob_c = (int)rd32(ert + 12);
    entry_count = (int)rd32(ert + 16);
    if (blob_len <= 0 || blob_len > BLOB_MAX || entry_count <= 0) {
        say(t("e.damaged"));
        return -1;
    }
    fs_lba = blob_len / 512;
    min_sectors = fs_lba + FS_MIN_SECTORS;
    return 0;
}

static int disk_fits(int i)
{
    return disk_sec[i] >= min_sectors;
}

static void install_begin(void)
{
    page = PAGE_INSTALL;
    step = 0;
    done_steps = 0;
    failed = 0;
    pos = 0;
    total_steps = 4 + entry_count;
    say(t("e.prep"));
    draw_page();
}

static int install_step(void)
{
    if (failed)
        return 1;

    if (step == 0) {
        say(t("e.read"), ert_len, entry_count);
    } else if (step == 1) {
        int got = lzss_decompress(ert + ERT_HEADER, blob_c, blob, blob_len);
        if (got != blob_len) {
            say(t("e.baddecomp"), got, blob_len);
            failed = 1;
        } else {
            say(t("e.unpack"), blob_len);
        }
    } else if (step == 2) {
        int r = sys_rawwrite(disk_name[cur_dev], 0, blob, blob_len / 512);
        if (r < 0) {
            say(t("e.writeraw"), disk_name[cur_dev], r);
            failed = 1;
        } else {
            say(t("e.bootwrite"));
        }
    } else if (step == 3) {
        int r;
        if (fs_choice == 0)
            r = sys_install(disk_name[cur_dev], fs_lba);
        else if (fs_choice == 1)
            r = sys_install_fat(disk_name[cur_dev], fs_lba, 12);
        else if (fs_choice == 2)
            r = sys_install_fat(disk_name[cur_dev], fs_lba, 16);
        else
            r = sys_install_fat(disk_name[cur_dev], fs_lba, 32);
        if (r == -2 || r == -3) {
            say(t("e.toosmall2"), disk_name[cur_dev], min_sectors / 2,
                disk_sec[cur_dev] / 2);
            failed = 1;
        } else if (r != 0) {
            say(t("e.mkfs"), disk_name[cur_dev], r);
            failed = 1;
        } else {
            say(t("e.created"), fs_names[fs_choice], fs_lba);
            pos = ERT_HEADER + blob_c;
            /* 持久化安装语言：系统盘 /LANG（en 或 zh），启动时由内核读取 */
            {
                const char *lg = (lang_cur == 1) ? "zh\n" : "en\n";
                int lfd = sys_open("/TARGET/LANG", 1);
                if (lfd >= 0) {
                    if (sys_writefd(lfd, lg, (int)strlen(lg)) == (int)strlen(lg))
                        sys_close(lfd);
                }
            }
            /* 中文语言包：用户勾选时把 /I18N/ZH.MSG 从安装源拷到目标盘；
             * 英文语言包 /I18N/EN.MSG 默认随系统快照装入。 */
            if (zh_pkg) {
                static u8 pkgbuf[4096];
                int pfd = sys_open("/I18N/ZH.MSG", 0);
                if (pfd >= 0) {
                    int n = sys_read(pfd, pkgbuf, sizeof(pkgbuf));
                    sys_close(pfd);
                    if (n > 0) {
                        int tfd;
                        sys_mkdir("/TARGET/I18N");
                        tfd = sys_open("/TARGET/I18N/ZH.MSG", 1);
                        if (tfd >= 0) {
                            if (sys_writefd(tfd, pkgbuf, n) != n)
                                sys_close(tfd);
                            else
                                sys_close(tfd);
                            say(t("e.copy"), "I18N/ZH.MSG", n);
                        }
                    }
                }
            }
        }
    } else {
        int idx = step - 4;
        int kind, nlen, rawsize, compsize;
        char name[200];
        char path[260];

        if (idx >= entry_count)
            return 1;
        if (pos + 12 > ert_len) {
            say(t("e.trunc"));
            failed = 1;
            return 1;
        }
        kind = ert[pos];
        nlen = (int)rd16(ert + pos + 2);
        rawsize = (int)rd32(ert + pos + 4);
        compsize = (int)rd32(ert + pos + 8);
        pos += 12;
        if (nlen > 190 || pos + nlen > ert_len || pos + nlen + compsize > ert_len) {
            say(t("e.badentry"), idx, pos, nlen, rawsize, compsize, ert_len);
            failed = 1;
            return 1;
        }
        for (int k = 0; k < nlen; k++)
            name[k] = (char)ert[pos + k];
        name[nlen] = 0;
        pos += nlen;
        snprintf(path, sizeof(path), "/TARGET/%s", name);

        if (kind == 1) {
            sys_mkdir(path);
            say(t("e.mkdir"), name);
        } else if (rawsize > FILE_MAX) {
            say(t("e.big"), name);
        } else {
            int got = lzss_decompress(ert + pos, compsize, filebuf, rawsize);
            int fd;
            if (got != rawsize) {
                say(t("e.decomp"), name);
                failed = 1;
            } else if ((fd = sys_open(path, 1)) < 0) {
                say(t("e.create"), name, fd);
                failed = 1;
            } else if (sys_writefd(fd, filebuf, rawsize) != rawsize) {
                say(t("e.write"), name);
                sys_close(fd);
                failed = 1;
            } else if (sys_close(fd) != 0) {
                say(t("e.flush"), name);
                failed = 1;
            } else {
                say(t("e.copy"), name, rawsize);
            }
        }
        pos += compsize;
    }

    step++;
    done_steps++;
    return step > total_steps - 1;
}

static void click(int id)
{
    if (id == 0) {
        say(t("e.cancel"));
        page = PAGE_DONE;
        failed = 0;
    } else if (id == 1) {
        disk_scan();
        for (int i = 0; i < disk_count; i++)
            if (disk_fits(i)) {
                cur_dev = i;
                break;
            }
        page = PAGE_TARGET;
    } else if (id == 2) {
        page = PAGE_WELCOME;
    } else if (id == 3) {
        install_begin();
    } else if (id == 4) {
        sys_reboot();
    }
}

static void handle_click(int mx, int my)
{
    if (page == PAGE_WELCOME) {
        for (int i = 0; i < 2; i++) {
            int x = 60 + i * 110;
            if (my >= 80 && my < 106 && mx >= x && mx < x + 102) {
                lang_cur = i;
                return;
            }
        }
    }
    if (page == PAGE_TARGET) {
        int fty = 106 + disk_count * 34 + 24;
        for (int i = 0; i < 4; i++) {
            int x = 60 + i * 132;
            if (my >= fty && my < fty + 26 && mx >= x && mx < x + 124) {
                fs_choice = i;
                return;
            }
        }
        for (int i = 0; i < disk_count; i++) {
            int y = 106 + i * 34;
            if (my >= y && my < y + 30 && mx >= 52 && mx < WIN_W - 52) {
                cur_dev = i;
                return;
            }
        }
    }
    if (my < BTN_Y || my >= BTN_Y + BTN_H)
        return;
    if (page == PAGE_WELCOME) {
        if (mx >= WIN_W - 2 * (BTN_W + 10) - 18 && mx < WIN_W - (BTN_W + 10) - 18)
            click(0);
        else if (mx >= WIN_W - (BTN_W + 10) - 18)
            click(1);
    } else if (page == PAGE_TARGET) {
        if (mx >= WIN_W - 3 * (BTN_W + 10) - 18 && mx < WIN_W - 2 * (BTN_W + 10) - 18)
            click(2);
        else if (mx >= WIN_W - (BTN_W + 10) - 18 && disk_count > 0)
            click(3);
    } else if (page == PAGE_DONE) {
        if (mx >= WIN_W - (BTN_W + 10) - 18)
            click(4);
    }
}

static void big_head(int cx, int cy, int r, int color)
{
    for (int dy = -r; dy <= r; dy++) {
        int rr = 0;
        while ((rr + 1) * (rr + 1) + dy * dy <= r * r)
            rr++;
        sys_rect(cx - rr, cy + dy, rr * 2, 1, color);
    }
}

static void draw_big_face(void)
{
    int cx = WIN_W / 2;
    int cy = 290;
    int r = 168;

    big_head(cx, cy, r, RGB(252, 214, 96));
    big_head(cx, cy, r - 4, RGB(255, 226, 128));
    big_head(cx - 62, cy - 40, 40, RGB(255, 255, 255));
    big_head(cx + 62, cy - 40, 40, RGB(255, 255, 255));
    big_head(cx - 62, cy - 36, 22, RGB(40, 40, 60));
    big_head(cx + 62, cy - 36, 22, RGB(40, 40, 60));
    big_head(cx - 56, cy - 46, 7, RGB(255, 255, 255));
    big_head(cx + 68, cy - 46, 7, RGB(255, 255, 255));
    for (int i = 0; i < 46; i++) {
        sys_rect(cx - 118 + i, cy - 104 + i / 3, 8, 12, RGB(60, 40, 30));
        sys_rect(cx + 110 - i, cy - 104 + i / 3, 8, 12, RGB(60, 40, 30));
    }
    for (int i = 0; i < 150; i++) {
        int y = cy + 74 + (i * i) / 260;
        if (y > cy + 128)
            break;
        sys_rect(cx - 75 + i, y, 2, 6, RGB(120, 40, 40));
    }
    big_head(cx - 118, cy + 34, 26, RGB(250, 150, 150));
    big_head(cx + 118, cy + 34, 26, RGB(250, 150, 150));
}

static void center(int y, const char *s, int color, int scale)
{
    int w = utf8_w(s) * scale;
    text((WIN_W - w) / 2, y, s, color, scale);
}

static void easter_egg(void)
{
    int start;

    have_win = (sys_win_open("Setup", WIN_W, WIN_H) == 0);
    if (have_win) {
        sys_clear(RGB(18, 24, 36));
        draw_big_face();
        center(30, "How dare you install a system", RGB(228, 40, 40), 3);
        center(66, "on the hard disk.", RGB(228, 40, 40), 3);
        center(108, "Bold!", RGB(255, 90, 60), 2);
        sys_flush();
    } else {
        printf("\nHow dare you install a system on the hard disk. Bold!\n");
    }

    start = sys_ticks();
    while (sys_ticks() - start < 1000) {
        sys_sleep(20);
        if (have_win)
            sys_pump();
    }
    sys_poweroff();
}

int main(void)
{
    int drive = sys_bootmode();

    if (drive >= 0x80) {
        printf("aptos setup: booted from hard disk (%x) - continuing, install to a second disk\n", drive);
    }

    have_win = (sys_win_open(t("win.title"), WIN_W, WIN_H) == 0);
    if (!have_win)
        printf("aptos 2.0 setup - no window available, using the console\n");

    if (read_snapshot_header() != 0) {
        snapshot_ok = 0;
        page = PAGE_DONE;
    } else {
        snapshot_ok = 1;
        page = PAGE_WELCOME;
    }
    draw_page();

    for (;;) {
        int mx = 0, my = 0, btn = 0, inside = 0;
        int k = sys_getkey();

        if (k == 27) {
            if (page == PAGE_WELCOME)
                break;
            if (page == PAGE_TARGET)
                page = PAGE_WELCOME;
        } else if (k >= '1' && k <= '2' && page == PAGE_WELCOME) {
            lang_cur = k - '1';
            draw_page();
        } else if (k == '\n' || k == '\r') {
            if (page == PAGE_WELCOME)
                click(1);
            else if (page == PAGE_TARGET)
                click(3);
            else if (page == PAGE_DONE)
                click(4);
        } else if ((k == 0x104 || k == 0x103) && page == PAGE_TARGET) {
            fs_choice = (k == 0x104) ? (fs_choice + 1) % 4 : (fs_choice + 3) % 4;
            draw_page();
        } else if ((k == 0x102 || k == 0x101) && page == PAGE_TARGET) {
            if (disk_count > 0) {
                cur_dev = (k == 0x102) ? (cur_dev + 1) % disk_count
                                       : (cur_dev + disk_count - 1) % disk_count;
                draw_page();
            }
        } else if (k >= '1' && k <= '4' && page == PAGE_TARGET) {
            fs_choice = k - '1';
            draw_page();
        } else if ((k == 'z' || k == 'Z') && page == PAGE_TARGET) {
            zh_pkg = !zh_pkg;
            draw_page();
        }

        if (page == PAGE_INSTALL && !failed) {
            int finished = install_step();
            if (finished && !failed)
                page = PAGE_DONE;
            draw_page();
            sys_sleep(10);
            continue;
        }

        sys_mouse(&mx, &my, &btn, &inside);
        if ((btn & MOUSE_BTN_LEFT) && !(prev_btn & MOUSE_BTN_LEFT) && inside) {
            handle_click(mx, my);
            draw_page();
        }
        prev_btn = btn;

        if (mx != last_mx || my != last_my) {
            draw_page();
            last_mx = mx;
            last_my = my;
        }
        sys_sleep(16);
    }

    return 0;
}
