#include "gui.h"
#include "acpi.h"
#include "ata.h"
#include "blkdev.h"
#include "bootinfo.h"
#include "fat.h"
#include "fs.h"
#include "heap.h"
#include "i18n.h"
#include "io.h"
#include "keyboard.h"
#include "heap.h"
#include "klog.h"
#include "lst.h"
#include "pkg.h"
#include "user.h"
#include "scheduler.h"
#include "rtc.h"
#include "timer.h"
#include "util.h"
#include "net.h"
#include "serial.h"

#define TERM_COLS 78
#define TERM_ROWS 30
#define TERM_W (TERM_COLS * 8)
#define TERM_H (TERM_ROWS * 10)
#define HIST_MAX 8

typedef struct {
    char lines[TERM_ROWS][TERM_COLS + 1];
    int  cx, cy;
    char inbuf[TERM_COLS + 1];
    int  inlen;
    int  incur;                     /* 输入光标（inbuf 内偏移） */
    char hist[HIST_MAX][TERM_COLS + 1];
    int  hist_count;
    int  hist_pos;
} term_t;

static term_t    term;
static window_t *win;
static char      file_buf[4096];

/* 输出重定向：cap_on 时 term_putc 把字符攒进 cap_buf，不上屏 */
#define CAP_MAX 16384
static char cap_buf[CAP_MAX];
static int  cap_len, cap_on, cap_full;

static void term_scroll(void)
{
    for (int i = 0; i < TERM_ROWS - 1; i++)
        memcpy(term.lines[i], term.lines[i + 1], TERM_COLS + 1);
    memset(term.lines[TERM_ROWS - 1], 0, TERM_COLS + 1);
    term.cy = TERM_ROWS - 1;
}

static void term_putc(int c)
{
    if (cap_on) {
        if (c == '\r')
            return;
        if (cap_len < CAP_MAX - 1)
            cap_buf[cap_len++] = (char)c;
        else
            cap_full = 1;
        return;
    }
    gui_dirty_window(win);                 /* 任何输出都要触发重绘，不能等鼠标动 */
    if (c == '\r')
        return;
    serial_putc(c);
    if (c == '\n') {
        term.cx = 0;
        term.cy++;
        if (term.cy >= TERM_ROWS)
            term_scroll();
        return;
    }
    if (c == '\r') {
        term.cx = 0;
        return;
    }
    if (c == '\t') {
        do {
            term_putc(' ');
        } while (term.cx % 8 && term.cx < TERM_COLS);
        return;
    }
    if (term.cx >= TERM_COLS) {
        term_putc('\n');
    }
    term.lines[term.cy][term.cx] = (char)c;
    if (term.cx + 1 <= TERM_COLS)
        term.lines[term.cy][term.cx + 1] = 0;
    term.cx++;
}

static void term_puts(const char *s)
{
    while (*s)
        term_putc(*s++);
}

static void term_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

static char cwd[128] = "/";

static void term_prompt(void)
{
    term_puts(cwd);
    term_puts("> ");
}

static void cmd_ls(const char *arg)
{
    char path[256];
    fs_dirent_t e;
    int i = 0, n = 0;

    fs_path(path, sizeof(path), cwd, arg && *arg ? arg : ".");
    if (!fs_is_dir(path)) {
        term_printf("ls: %s: not a directory\n", path);
        return;
    }

    term_printf("directory %s\n", path);
    term_printf("%-20s %9s  %s\n", "NAME", "SIZE", "DATE");
    while (fs_list(path, i++, &e) == 0) {
        if (e.flags & FS_FLAG_DIR)
            term_printf("%-20s %9s  <DIR>\n", e.name, "-");
        else
            term_printf("%-20s %9d  %08d\n", e.name, e.size, e.mdate);
        n++;
    }
    term_printf("%d entr%s, %d KiB free\n", n, n == 1 ? "y" : "ies", fs_free_kb());
}

static void cmd_cd(const char *arg)
{
    char path[256];

    fs_path(path, sizeof(path), cwd, arg && *arg ? arg : "/");
    if (!fs_is_dir(path)) {
        term_printf("cd: %s: no such directory\n", path);
        return;
    }
    strncpy(cwd, path, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
}

static void cmd_pwd(void)
{
    term_printf("%s\n", cwd);
}

static void cmd_cat(const char *arg)
{
    char path[256];
    int len;

    if (!arg || !*arg) {
        term_puts(L("term.usecat"));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    if (fs_is_dir(path)) {
        term_printf("cat: %s: is a directory\n", path);
        return;
    }
    len = fs_read(path, file_buf, sizeof(file_buf) - 1);
    if (len < 0) {
        term_printf("cat: %s: no such file\n", path);
        return;
    }
    file_buf[len] = 0;
    for (int i = 0; i < len; i++) {
        char c = file_buf[i];
        term_putc(c == '\t' ? ' ' : c);
    }
    if (len && file_buf[len - 1] != '\n')
        term_putc('\n');
}

static void cmd_write(const char *name, const char *text)
{
    char path[256];
    int n;

    if (!name || !*name) {
        term_puts(L("term.usewrite"));
        return;
    }
    fs_path(path, sizeof(path), cwd, name);
    n = fs_write(path, text ? text : "", text ? (u32)strlen(text) : 0);
    if (n < 0)
        term_printf("write: could not save %s\n", path);
    else
        term_printf(L("term.wrote"), n, path);
}

static void cmd_edit(const char *arg)
{
    char path[256];

    if (arg && *arg) {
        fs_path(path, sizeof(path), cwd, arg);
    } else {
        strcpy(path, "/UNTITLED.TXT");
    }
    app_editor_load(path);
    term_printf("edit: %s\n", path);
}

static void cmd_headtail(const char *arg, int tail)
{
    char path[256];
    int n = 10, i, total, lines = 0, start = 0, seen = 0;
    const char *p = arg;

    if (!p || !*p) {
        term_puts(tail ? "usage: tail [n] file\n" : "usage: head [n] file\n");
        return;
    }
    if (*p >= '0' && *p <= '9') {
        n = 0;
        while (*p >= '0' && *p <= '9')
            n = n * 10 + (*p++ - '0');
        while (*p == ' ')
            p++;
    }
    if (!*p) {
        term_puts(tail ? "usage: tail [n] file\n" : "usage: head [n] file\n");
        return;
    }
    fs_path(path, sizeof(path), cwd, p);
    total = fs_read(path, file_buf, sizeof(file_buf) - 1);
    if (total <= 0) {
        term_printf("%s: cannot read\n", path);
        return;
    }
    file_buf[total] = 0;

    for (i = 0; i < total; i++)
        if (file_buf[i] == '\n')
            lines++;
    if (file_buf[total - 1] != '\n')
        lines++;

    if (!tail) {
        int cnt = 0;
        for (i = 0; i < total && cnt < n; i++) {
            term_putc(file_buf[i]);
            if (file_buf[i] == '\n')
                cnt++;
        }
        if (i > 0 && file_buf[i - 1] != '\n')
            term_putc('\n');
    } else {
        if (lines > n) {
            int skip = lines - n;
            for (i = 0; i < total; i++) {
                if (file_buf[i] == '\n') {
                    seen++;
                    if (seen == skip) {
                        start = i + 1;
                        break;
                    }
                }
            }
        }
        for (i = start; i < total; i++)
            term_putc(file_buf[i]);
        if (total > start && file_buf[total - 1] != '\n')
            term_putc('\n');
    }
}

static void cmd_wc(const char *arg)
{
    char path[256];
    int n, i, lines = 0, words = 0, inword = 0;

    if (!arg || !*arg) {
        term_puts("usage: wc file\n");
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    n = fs_read(path, file_buf, sizeof(file_buf) - 1);
    if (n <= 0) {
        term_printf("%s: cannot read\n", path);
        return;
    }
    for (i = 0; i < n; i++) {
        char c = file_buf[i];
        if (c == '\n')
            lines++;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            inword = 0;
        } else if (!inword) {
            inword = 1;
            words++;
        }
    }
    if (file_buf[n - 1] != '\n')
        lines++;
    term_printf("%d %d %d %s\n", lines, words, n, path);
}

static void cmd_mkdir(const char *arg)
{
    char path[256];

    if (!arg || !*arg) {
        term_puts(L("term.usemkdir"));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    if (fs_mkdir(path) == 0)
        term_printf(L("term.creatdir"), path);
    else
        term_printf("mkdir: cannot create %s\n", path);
}

static void cmd_rm(const char *arg)
{
    char path[256];
    int r;

    if (!arg || !*arg) {
        term_puts(L("term.userm"));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    r = fs_delete(path);
    if (r == 0)
        term_printf(L("term.removed"), path);
    else if (r == -2)
        term_printf("rm: %s: directory not empty\n", path);
    else
        term_printf("rm: %s: no such file or directory\n", path);
}

static void cmd_cp(const char *from, const char *to)
{
    char a[256], b[256];
    int len;

    if (!from || !to) {
        term_puts(L("term.usecp"));
        return;
    }
    fs_path(a, sizeof(a), cwd, from);
    fs_path(b, sizeof(b), cwd, to);
    if (fs_is_dir(b)) {
        char joined[256];
        snprintf(joined, sizeof(joined), "%s/%s", b, fs_basename(a));
        strncpy(b, joined, sizeof(b) - 1);
        b[sizeof(b) - 1] = 0;
    }
    len = fs_read(a, file_buf, sizeof(file_buf) - 1);
    if (len < 0) {
        term_printf("cp: %s: no such file\n", a);
        return;
    }
    if (fs_write(b, file_buf, (u32)len) < 0) {
        term_printf("cp: cannot write %s\n", b);
        return;
    }
    term_printf(L("term.copied"), a, b, len);
}

static void cmd_mv(const char *from, const char *to)
{
    char a[256], b[256];
    int r;

    if (!from || !to) {
        term_puts(L("term.usemv"));
        return;
    }
    fs_path(a, sizeof(a), cwd, from);
    fs_path(b, sizeof(b), cwd, to);
    if (fs_is_dir(b)) {
        char joined[256];
        snprintf(joined, sizeof(joined), "%s/%s", b, fs_basename(a));
        strncpy(b, joined, sizeof(b) - 1);
        b[sizeof(b) - 1] = 0;
    }
    r = fs_rename(a, b);
    if (r == 0)
        term_printf(L("term.moved"), a, b);
    else if (r == -2)
        term_printf("mv: %s: destination exists\n", b);
    else
        term_printf("mv: %s: no such file or directory\n", a);
}

static void cmd_dmesg(void)
{
    int n = klog_count();
    for (int i = 0; i < n; i++)
        term_printf("%s\n", klog_line(i));
}

static void cmd_help(void)
{
    static const char *help[] = {
        "help              this list",
        "clear             clear the screen",
        "echo <text>       print text",
        "ls [path]         list a directory",
        "cd <path> / pwd   walk the tree",
        "cat <file>        show a file",
        "write <f> <text>  create/overwrite a file",
        "edit [file]       open the editor on a file",
        "head [n] <file>   first n lines (default 10)",
        "tail [n] <file>   last n lines (default 10)",
        "wc <file>         lines / words / bytes",
        "cmd > file        redirect output (> appends with >>)",
        "mkdir <name>      create a directory",
        "rm <name>         delete file or empty dir",
        "cp <src> <dst>    copy a file",
        "mv <src> <dst>    rename or move",
        "df                filesystem usage",
        "mem               memory usage",
        "dmesg             kernel boot log",
        "date / time       hardware clock",
        "uptime            ticks since boot",
        "ver               version banner",
        "gfx               display information",
        "install <pkg.lst> open a package and run its installer",
        "pkginfo <pkg.lst> show what a package contains",
        "run <file> [&]    execute an ELF32 binary in ring 3 (& = background)",
        "jobs              list background tasks",
        "kill <id>         kill a background task",
        "memtest [mb]      allocate and verify that much RAM (default 16)",
        "acpi              ACPI tables and power management",
        "dev               list block devices",
        "mounts            list mounted filesystems",
        "mount <dev> <dir> mount a filesystem (hd0/hd1/cd0/fd0)",
        "umount <dir>      unmount a filesystem",
        "mkfs <dev>        format a device as MINIFS02",
        "apps              list the applications",
        "files|edit <f>|paint|about   open a window",
        "snake|mines|2048  play a game",
        "reboot            restart (8042 reset)",
        "poweroff          ACPI S5 power off",
    };
    for (unsigned i = 0; i < ARRAY_LEN(help); i++)
        term_printf("%s\n", help[i]);
}

static void cmd_gfx(void)
{
    term_printf("display : %dx%d, %d bpp\n", gfx_width(), gfx_height(),
                (int)gfx_bpp());
    term_printf("fb      : 0x%x, pitch %d\n", BOOTINFO->fb_addr, BOOTINFO->fb_pitch);
}

static void install_package(const char *path)
{
    int r;

    if (!fs_exists(path)) {
        term_printf("install: %s: no such package\n", path);
        return;
    }
    term_printf(L("term.opening"), path);
    gui_dirty_window(win);
    r = pkg_run(path);
    if (r == 0)
        term_puts(L("term.installfin"));
    else
        term_printf(L("term.installfail"), r);
}

static void cmd_install(const char *arg)
{
    char path[256];

    if (!arg || !*arg) {
        term_puts(L("term.useinstall"));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    install_package(path);
}

void term_install_package(const char *path)
{
    install_package(path);
}

static void cmd_pkginfo(const char *arg)
{
    char path[256];
    const u8 *data;
    int len;
    const lst_header_t *h;
    int i;

    if (!arg || !*arg) {
        term_puts(L("term.usepkginfo"));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    len = fs_read(path, file_buf, sizeof(file_buf) - 1);
    if (len < 0) {
        term_printf("pkginfo: %s: no such file\n", path);
        return;
    }
    data = (const u8 *)file_buf;
    if (!lst_validate(data, (u32)len)) {
        term_printf("pkginfo: %s: not a valid .lst package\n", path);
        return;
    }
    h = lst_header(data);
    term_printf("package    : %s\n", h->name);
    term_printf("vendor     : %s\n", h->vendor);
    term_printf("description: %s\n", h->description);
    term_printf("entries    : %d (installer is #%d)\n", h->count, h->installer);
    for (i = 0; i < (int)h->count; i++) {
        const lst_entry_t *e = lst_entry(data, (u32)len, (u32)i);
        if (!e)
            continue;
        term_printf("  %-20s %7d bytes%s\n", e->name, e->size,
                    (u32)i == h->installer ? "   <- installer" : "");
    }
    term_puts("run 'install <file.lst>' to install it\n");
}

static void cmd_ping(const char *arg)
{
    u32 ip;
    int ok = 0, i, n = 4;
    u32 avg = 0;
    char tmp[64];

    if (!arg || !*arg) {
        term_puts(L("term.useping"));
        return;
    }
    {
        char ipbuf[40];
        int j = 0;
        while (arg[j] && arg[j] != ' ' && j < 39) {
            ipbuf[j] = arg[j];
            j++;
        }
        ipbuf[j] = 0;
        ip = net_aton(ipbuf);
        if (ip == 0) {
            /* try hostname via DNS */
            if (net_dns_query(ipbuf, &ip) != 0) {
                term_puts("ping: bad ip or unknown host\n");
                return;
            }
        }
        if (arg[j] == ' ') {
            n = atoi(&arg[j + 1]);
            if (n < 1)
                n = 1;
            if (n > 10)
                n = 10;
        }
    }
    net_ntoa(ip, tmp);
    if (!net_up()) {
        term_puts("ping: network is down\n");
        return;
    }
    for (i = 0; i < n; i++) {
        u32 t0 = timer_uptime_ms();
        int got = 0;
        {
            /* one shot: use net_ping with count 1 */
            int o2 = 0;
            u32 a2 = 0;
            net_ping(ip, 1, 1500, &o2, &a2);
            got = o2;
            avg += a2;
        }
        if (got) {
            term_printf("ping %s: seq %d ok rtt ~%dms\n", tmp, i + 1, (u32)(timer_uptime_ms() - t0));
            ok++;
        } else {
            term_printf("ping %s: seq %d timeout\n", tmp, i + 1);
        }
        if (i < n - 1)
            timer_sleep_ms(200);
    }
    term_printf("ping %s: %d/%d received\n", tmp, ok, n);
}

/* rx <path> <bytes>: read raw bytes from COM1 into a file */
static void cmd_rx(const char *arg)
{
    char path[160];
    u32 want = 0;
    u32 got = 0;
    u8 *buf;
    int n = 0;
    const char *p = arg;

    if (!arg || !*arg) {
        term_puts("rx: usage rx <path> <bytes>\n");
        return;
    }
    while (*p && *p != ' ' && n < 159)
        path[n++] = *p++;
    path[n] = 0;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9') {
        want = want * 10 + (u32)(*p - '0');
        p++;
    }
    if (!want) {
        term_puts("rx: need byte count\n");
        return;
    }
    term_printf("rx: waiting %u bytes -> %s\n", want, path);
    buf = (u8 *)kmalloc(want);
    if (!buf) {
        term_puts("rx: out of memory\n");
        return;
    }
    while (got < want) {
        int c = serial_getc();
        if (c < 0)
            continue;
        buf[got++] = (u8)c;
    }
    if (fs_write(path, buf, got) < 0) {
        term_puts("rx: write failed\n");
        return;
    }
    term_printf("rx: wrote %u bytes\n", got);
}

static void cmd_netstat(const char *arg)
{
    netinfo_t ni;
    (void)arg;
    net_getinfo(&ni);
    term_printf("IP %d.%d.%d.%d\n", (u8)ni.ip, (u8)(ni.ip >> 8), (u8)(ni.ip >> 16), (u8)(ni.ip >> 24));
    term_printf("gw %d.%d.%d.%d\n", (u8)ni.gw, (u8)(ni.gw >> 8), (u8)(ni.gw >> 16), (u8)(ni.gw >> 24));
    term_printf("dns %d.%d.%d.%d\n", (u8)ni.dns, (u8)(ni.dns >> 8), (u8)(ni.dns >> 16), (u8)(ni.dns >> 24));
    term_printf("mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    term_printf("link %s\n", ni.up ? "up" : "down");
}

static void cmd_dns(const char *arg)
{
    u32 ip;
    char tmp[32];
    if (!arg || !*arg) {
        term_puts(L("term.usedns"));
        return;
    }
    if (net_dns_query(arg, &ip) == 0) {
        net_ntoa(ip, tmp);
        term_printf("%s -> %s\n", arg, tmp);
    } else {
        term_printf("dns: no answer for %s\n", arg);
    }
}

static u8 *exec_scratch;

static void run_program(const char *path, const char *arg1);

/* "name loops" -> a0="name" a1="loops"；单个词 -> a1=word（如 javm 的类路径） */
static void split_param(const char *p, char *a0, char *a1, int sz)
{
    int i = 0, j;
    a0[0] = a1[0] = 0;
    if (!p || !*p)
        return;
    while (p[i] && p[i] != ' ' && i < sz - 1) {
        a0[i] = p[i];
        i++;
    }
    a0[i] = 0;
    while (p[i] == ' ')
        i++;
    if (p[i]) {
        j = 0;
        while (p[i] && j < sz - 1) {
            a1[j] = p[i];
            i++;
            j++;
        }
        a1[j] = 0;
    } else {
        /* 只有一个词：放进 a1（arg1），a0 留空 */
        for (j = 0; a0[j]; j++)
            a1[j] = a0[j];
        a1[j] = 0;
        a0[0] = 0;
    }
}
static int run_program_bg(const char *path, const char *arg1);
static void cmd_run(const char *arg);

#include <stdarg.h>

void term_say(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (char *p = buf; *p; p++)
        term_putc(*p);
    gui_dirty_window(win);
}

void term_run_program(const char *path)
{
    run_program(path, NULL);
}

static void cmd_run(const char *arg)
{
    char path[256];
    char param[256];
    const char *ap = arg;
    int n = 0;

    if (!arg || !*arg) {
        term_puts(L("term.userun"));
        return;
    }
    while (*ap && *ap != ' ') {
        if (n < 255)
            path[n++] = *ap;
        ap++;
    }
    path[n] = 0;
    if (*ap == ' ') {
        ap++;
        n = 0;
        while (*ap) {
            if (n < 255)
                param[n++] = *ap;
            ap++;
        }
        param[n] = 0;
    } else {
        param[0] = 0;
    }
    {
        int bg = 0;
        int plen = (int)strlen(param);
        if (plen > 0 && param[plen - 1] == '&') {
            bg = 1;
            param[plen - 1] = 0;
        }
        fs_path(path, sizeof(path), cwd, path);
        if (!fs_exists(path)) {
            char alt[300];
            snprintf(alt, sizeof(alt), "%s.APP", path);
            if (fs_exists(alt))
                strcpy(path, alt);
        }
        if (bg)
            run_program_bg(path, param[0] ? param : NULL);
        else
            run_program(path, param[0] ? param : NULL);
    }
}

static void run_program(const char *path, const char *arg1)
{
    int len;
    int tid, st;

    if (!exec_scratch) {
        exec_scratch = (u8 *)kmalloc(512 * 1024);
        if (!exec_scratch) {
            term_puts("run: out of memory\n");
            return;
        }
    }
    len = fs_read(path, exec_scratch, 512 * 1024);
    if (len < 0) {
        term_printf("run: %s: no such file\n", path);
        return;
    }
    {
        char a0[160], a1[160];
        split_param(arg1, a0, a1, sizeof(a0));
        tid = scheduler_spawn(exec_scratch, (u32)len, path, a0, a1);
    }
    if (tid < 0) {
        term_printf("run: spawn failed (%d)\n", tid);
        return;
    }
    term_printf(L("term.running"), path, len);
    term_printf("  [task %d]\n", tid);
    gui_dirty_window(win);
    /* 前台等待任务退出；期间其他任务由 PIT 抢占推进 */
    while (scheduler_alive(tid)) {
        gui_poll();
        timer_sleep_ms(50);
    }
    st = scheduler_status(tid);
    scheduler_reap_zombie(tid);
    term_printf(L("term.exited"), st);
}

/* 后台运行：spawn 后立即返回 */
static int run_program_bg(const char *path, const char *arg1)
{
    int len;
    int tid;

    if (!exec_scratch) {
        exec_scratch = (u8 *)kmalloc(512 * 1024);
        if (!exec_scratch) {
            term_puts("run: out of memory\n");
            return -1;
        }
    }
    len = fs_read(path, exec_scratch, 512 * 1024);

    if (len < 0) {
        term_printf("run: %s: no such file\n", path);
        return -1;
    }
    {
        char a0[160], a1[160];
        split_param(arg1, a0, a1, sizeof(a0));
        tid = scheduler_spawn(exec_scratch, (u32)len, path, a0, a1);
    }
    if (tid < 0) {
        term_printf("run: spawn failed (%d)\n", tid);
        return -1;
    }
    term_printf(L("term.running"), path, len);
    term_printf("  [task %d in background]\n", tid);
    gui_dirty_window(win);
    return tid;
}

static void cmd_jobs(const char *arg)
{
    (void)arg;
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (scheduler_alive(i)) {
            term_printf("%d  %-24s running\n", i, scheduler_name(i));
            n++;
        }
    }
    if (n == 0)
        term_puts("jobs: no background tasks\n");
}

static void cmd_kill(const char *arg)
{
    int id;
    if (!arg || !*arg) {
        term_puts("kill: usage: kill <task id>\n");
        return;
    }
    id = atoi(arg);
    if (id < 0 || id >= MAX_TASKS || scheduler_kill(id) != 0) {
        term_printf("kill: no such task %d\n", id);
        return;
    }
    term_printf("killed task %d\n", id);
}



static void cmd_memtest(const char *arg)
{
    u32 mb = (arg && *arg) ? (u32)atoi(arg) : 16;
    u32 bytes, off;
    u8 *p;
    int ok = 1;

    if (mb < 1)
        mb = 1;
    if (mb > 900)
        mb = 900;
    bytes = mb * 1024u * 1024u;

    term_printf("allocating %d MiB (0x%x bytes)...\n", mb, bytes);
    gui_dirty_window(win);
    p = (u8 *)kmalloc(bytes);
    if (!p) {
        term_printf("allocation failed - heap %d KiB used of %d MiB\n", heap_used() / 1024,
                    heap_total() / (1024 * 1024));
        return;
    }

    for (off = 0; off < bytes; off += 4096)
        p[off] = (u8)(off >> 12);
    for (off = 0; off < bytes; off += 4096) {
        if (p[off] != (u8)(off >> 12)) {
            term_printf("MISMATCH at +0x%x\n", off);
            ok = 0;
            break;
        }
    }

    term_printf("heap base 0x%x, block at 0x%x (+%d MiB into the heap)\n", heap_base(),
                (u32)p, ((u32)p - heap_base()) / (1024 * 1024));
    term_printf("every 4 KiB page written and read back: %s\n", ok ? "OK" : "FAILED");
    term_printf("heap now: %d KiB used of %d MiB\n", heap_used() / 1024,
                heap_total() / (1024 * 1024));
}

static void cmd_acpi(void)
{
    const acpi_info_t *a = acpi_info();
    void *tbl;
    u32 len;

    if (!a->present) {
        term_puts("ACPI: not available\n");
        return;
    }
    term_printf("ACPI revision : %d (%s)\n", a->revision,
                a->revision >= 2 ? "XSDT" : "RSDT");
    term_printf("OEM id        : '%s'  table id '%s'\n", a->oem_id, a->oem_table_id);
    term_printf("tables        : %d\n", a->table_count);
    term_printf("RSDT          : 0x%x\n", (u32)a->rsdt_addr);
    if (a->xsdt_addr)
        term_printf("XSDT          : 0x%x\n", (u32)a->xsdt_addr);
    term_printf("DSDT          : 0x%x\n", a->dsdt_addr);
    term_printf("cpus          : %d (lapic at 0x%x, %d ioapic)\n", a->cpus,
                a->lapic_addr, a->ioapics);
    term_printf("PM1a_CNT_BLK : 0x%x   PM1b 0x%x\n", a->pm1a_cnt, a->pm1b_cnt);

    if (acpi_find_table("FACS", &tbl, &len) == 0)
        term_printf("found FACS    : 0x%x (%d bytes)\n", (u32)tbl, len);
    term_puts("poweroff works through ACPI S5\n");
}

static void cmd_dev(void)
{
    int n = blk_count();

    term_printf("%-5s %-7s %10s %8s  %s\n", "NAME", "TYPE", "SECTORS", "SIZE", "MODEL");
    for (int i = 0; i < n; i++) {
        const blk_info_t *bi = blk_info(i);
        char sz[16];
        u32 kb = bi->sectors * bi->sector_size / 1024;
        if (kb >= 1024)
            snprintf(sz, sizeof(sz), "%d MiB", kb / 1024);
        else
            snprintf(sz, sizeof(sz), "%d KiB", kb);
        term_printf("%-5s %-7s %10d %8s  %s%s\n", bi->name, blk_type_name(bi->type),
                    bi->sectors, sz, bi->model, bi->writable ? "" : " (ro)");
    }
    if (n == 0)
        term_puts("no block devices\n");
}

static void cmd_mounts(void)
{
    fs_mountinfo_t mi[8];
    int n = fs_mounts(mi, 8);

    term_printf("%-14s %-5s %-8s %10s %10s  %s\n", "POINT", "DEV", "TYPE", "TOTAL",
                "FREE", "LABEL");
    for (int i = 0; i < n; i++)
        term_printf("%-14s %-5s %-8s %9dK %9dK  %s%s\n", mi[i].point, mi[i].dev,
                    fs_type_name(mi[i].type), mi[i].total_kb, mi[i].free_kb,
                    mi[i].label, mi[i].writable ? "" : " (ro)");
}

static void cmd_mount(const char *dev, const char *point)
{
    int r;

    if (!dev || !point) {
        term_puts(L("term.usemount"));
        return;
    }
    r = fs_mount(dev, point);
    if (r == 0)
        term_printf("mounted %s on %s\n", dev, point);
    else if (r == -2)
        term_printf("mount: %s: no such device (try 'dev')\n", dev);
    else if (r == -3)
        term_puts("mount: mount point must be an absolute path\n");
    else if (r == -4)
        term_printf("mount: %s: mount point already in use\n", point);
    else if (r == -6)
        term_printf("mount: %s: no MINIFS, FAT or ISO9660 filesystem found\n", dev);
    else if (r == -8)
        term_printf("mount: %s: device is already mounted\n", dev);
    else
        term_printf("mount: failed (%d)\n", r);
}

static void cmd_umount(const char *point)
{
    int r;
    if (!point || !*point) {
        term_puts(L("term.useumount"));
        return;
    }
    r = fs_umount(point);
    if (r == 0)
        term_printf("unmounted %s\n", point);
    else if (r == -2)
        term_puts("umount: cannot unmount the root filesystem\n");
    else
        term_printf("umount: %s: not mounted\n", point);
}

static void cmd_mkfs(const char *dev, const char *fsarg)
{
    int r;
    int want = 0;

    if (!dev || !*dev) {
        term_puts(L("term.usemkfs"));
        return;
    }
    if (fsarg) {
        if (strcasecmp(fsarg, "minifs") == 0)
            want = 0;
        else if (strcasecmp(fsarg, "fat12") == 0)
            want = FAT_TYPE_12;
        else if (strcasecmp(fsarg, "fat16") == 0)
            want = FAT_TYPE_16;
        else if (strcasecmp(fsarg, "fat32") == 0)
            want = FAT_TYPE_32;
        else {
            term_printf("mkfs: unknown filesystem '%s' (minifs/fat12/fat16/fat32)\n", fsarg);
            return;
        }
    }
    term_printf("formatting %s, this erases everything on it...\n", dev);
    if (want)
        r = fs_format_fat(dev, "aptos", want);
    else
        r = fs_format(dev, "aptos");
    if (r == 0) {
        if (want == FAT_TYPE_12)
            term_printf("%s formatted as FAT12\n", dev);
        else if (want == FAT_TYPE_16)
            term_printf("%s formatted as FAT16\n", dev);
        else if (want == FAT_TYPE_32)
            term_printf("%s formatted as FAT32\n", dev);
        else
            term_printf("%s formatted as MINIFS02\n", dev);
    } else if (r == -2)
        term_printf("mkfs: %s: no such device\n", dev);
    else if (r == -3)
        term_printf("mkfs: %s: read-only or unsupported device\n", dev);
    else if (r == -4)
        term_printf("mkfs: %s: unmount it first\n", dev);
    else
        term_printf("mkfs: failed (%d)\n", r);
}

static const struct {
    const char *name;
    const app_t *app;
} app_table[] = {
    { "terminal", &app_terminal },
    { "files",    &app_files    },
    { "editor",   &app_editor   },
    { "paint",    &app_paint    },
    { "about",    &app_about    },
    { "snake",    &app_snake    },
    { "mines",    &app_mines    },
    { "2048",     &app_2048     },
};

static void cmd_apps(void)
{
    term_puts(L("term.apps"));
    for (unsigned i = 0; i < ARRAY_LEN(app_table); i++)
        term_printf(" %s", app_table[i].name);
    term_puts("\n");
}

static const app_t *find_app(const char *name)
{
    for (unsigned i = 0; i < ARRAY_LEN(app_table); i++)
        if (strcasecmp(app_table[i].name, name) == 0)
            return app_table[i].app;
    return NULL;
}

static void run_command_body(char *line)
{
    while (*line == ' ')
        line++;
    if (!*line)
        return;

    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ')
            arg++;
    }

    if (strcasecmp(line, "help") == 0) {
        cmd_help();
    } else if (strcasecmp(line, "clear") == 0) {
        memset(term.lines, 0, sizeof(term.lines));
        term.cx = term.cy = 0;
    } else if (strcasecmp(line, "echo") == 0) {
        term_printf("%s\n", arg ? arg : "");
    } else if (strcasecmp(line, "ls") == 0 || strcasecmp(line, "dir") == 0) {
        cmd_ls(arg);
    } else if (strcasecmp(line, "cd") == 0) {
        cmd_cd(arg);
    } else if (strcasecmp(line, "pwd") == 0) {
        cmd_pwd();
    } else if (strcasecmp(line, "mkdir") == 0 || strcasecmp(line, "md") == 0) {
        cmd_mkdir(arg);
    } else if (strcasecmp(line, "cp") == 0 || strcasecmp(line, "copy") == 0) {
        char *second = arg ? strchr(arg, ' ') : NULL;
        if (second) {
            *second++ = 0;
            while (*second == ' ')
                second++;
        }
        cmd_cp(arg, second);
    } else if (strcasecmp(line, "mv") == 0 || strcasecmp(line, "ren") == 0) {
        char *second = arg ? strchr(arg, ' ') : NULL;
        if (second) {
            *second++ = 0;
            while (*second == ' ')
                second++;
        }
        cmd_mv(arg, second);
    } else if (strcasecmp(line, "cat") == 0 || strcasecmp(line, "type") == 0) {
        cmd_cat(arg);
    } else if (strcasecmp(line, "write") == 0) {
        char *text = arg ? strchr(arg, ' ') : NULL;
        if (text) {
            *text++ = 0;
            cmd_write(arg, text);
        } else {
            cmd_write(arg, "");
        }
    } else if (strcasecmp(line, "edit") == 0) {
        cmd_edit(arg);
    } else if (strcasecmp(line, "head") == 0) {
        cmd_headtail(arg, 0);
    } else if (strcasecmp(line, "tail") == 0) {
        cmd_headtail(arg, 1);
    } else if (strcasecmp(line, "wc") == 0) {
        cmd_wc(arg);
    } else if (strcasecmp(line, "rm") == 0 || strcasecmp(line, "del") == 0) {
        cmd_rm(arg);
    } else if (strcasecmp(line, "df") == 0) {
        fs_stat_t st;
        fs_stat(&st);
        term_printf("volume %s: %d KiB total, %d KiB free, %d files, %d dirs\n",
                    fs_volume_label(), st.total_kb, st.free_kb, st.files, st.dirs);
    } else if (strcasecmp(line, "mem") == 0) {
        term_printf("RAM: %d MiB total, heap used %d KiB of %d KiB\n",
                    mem_total_kb() / 1024, heap_used() / 1024, heap_total() / 1024);
    } else if (strcasecmp(line, "dmesg") == 0) {
        cmd_dmesg();
    } else if (strcasecmp(line, "date") == 0) {
        rtc_time_t t;
        rtc_read(&t);
        term_printf("%04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day,
                    t.hour, t.minute, t.second);
    } else if (strcasecmp(line, "time") == 0) {
        rtc_time_t t;
        rtc_read(&t);
        term_printf("%02d:%02d:%02d\n", t.hour, t.minute, t.second);
    } else if (strcasecmp(line, "uptime") == 0) {
        term_printf("up %d.%03d s (%d ticks at %d Hz)\n", timer_uptime_ms() / 1000,
                    timer_uptime_ms() % 1000, timer_ticks(), timer_hz());
    } else if (strcasecmp(line, "ver") == 0) {
        term_puts(L("term.ver"));
    } else if (strcasecmp(line, "gfx") == 0) {
        cmd_gfx();
    } else if (strcasecmp(line, "install") == 0) {
        cmd_install(arg);
    } else if (strcasecmp(line, "pkginfo") == 0) {
        cmd_pkginfo(arg);
    } else if (strcasecmp(line, "ping") == 0) {
        cmd_ping(arg);
    } else if (strcasecmp(line, "netstat") == 0) {
        cmd_netstat(arg);
    } else if (strcasecmp(line, "rx") == 0) {
        cmd_rx(arg);
    } else if (strcasecmp(line, "dns") == 0) {
        cmd_dns(arg);
    } else if (strcasecmp(line, "run") == 0) {
        cmd_run(arg);
    } else if (strcasecmp(line, "jobs") == 0) {
        cmd_jobs(arg);
    } else if (strcasecmp(line, "kill") == 0) {
        cmd_kill(arg);
    } else if (strcasecmp(line, "memtest") == 0) {
        cmd_memtest(arg);
    } else if (strcasecmp(line, "acpi") == 0) {
        cmd_acpi();
    } else if (strcasecmp(line, "disk") == 0 && 0) {
        cmd_dev();
    } else if (strcasecmp(line, "dev") == 0 || strcasecmp(line, "disk") == 0) {
        cmd_dev();
    } else if (strcasecmp(line, "mounts") == 0) {
        cmd_mounts();
    } else if (strcasecmp(line, "mount") == 0) {
        char *second = arg ? strchr(arg, ' ') : NULL;
        if (second) {
            *second++ = 0;
            while (*second == ' ')
                second++;
        }
        cmd_mount(arg, second);
    } else if (strcasecmp(line, "umount") == 0 || strcasecmp(line, "unmount") == 0) {
        cmd_umount(arg);
    } else if (strcasecmp(line, "mkfs") == 0 || strcasecmp(line, "format") == 0) {
        char *second = arg ? strchr(arg, ' ') : NULL;
        if (second) {
            *second++ = 0;
            while (*second == ' ')
                second++;
        }
        cmd_mkfs(arg, second);
    } else if (strcasecmp(line, "apps") == 0) {
        cmd_apps();
    } else if (strcasecmp(line, "lang") == 0 || strcasecmp(line, "language") == 0) {
        if (arg && *arg) {
            if (strcasecmp(arg, "list") == 0 || strcasecmp(arg, "l") == 0) {
                term_printf("available languages (%d):\n", i18n_count());
                for (int i = 0; i < i18n_count(); i++) {
                    const i18n_lang_t *lg = i18n_lang_at(i);
                    term_printf("  %s (%s)%s\n", lg->id, lg->native,
                                strcmp(lg->id, i18n_id()) == 0 ? " *" : "");
                }
            } else if (i18n_set(arg) == 0) {
                term_printf("language set to %s (%s)\n", i18n_id(), i18n_native());
            } else {
                term_printf("unknown language '%s' (try 'lang list')\n", arg);
            }
        } else {
            term_printf("current language: %s (%s)\n", i18n_id(), i18n_native());
        }
    } else if (strcasecmp(line, "reboot") == 0) {
        term_puts(L("term.reboot"));
        outb(0x64, 0xFE);
    } else if (strcasecmp(line, "poweroff") == 0 || strcasecmp(line, "halt") == 0 ||
               strcasecmp(line, "shutdown") == 0) {
        term_puts(L("term.poweroff"));
        gui_dirty_window(win);
        acpi_shutdown();
        cli();
        for (;;)
            hlt();
    } else if (strcasecmp(line, "edit") == 0) {
        char path[256];
        if (arg && *arg) {
            fs_path(path, sizeof(path), cwd, arg);
            app_editor_load(path);
        } else {
            app_editor_load("/UNTITLED.TXT");
        }
    } else {
        const app_t *a = find_app(line);
        if (a) {
            gui_open(a);
            term_printf(L("term.opened"), line);
        } else {
            term_printf(L("term.notfoundline"), line, L("term.notfound"));
        }
    }
}

/* 输出重定向：cmd > file / cmd >> file（捕获 term_putc，命令跑完再落盘） */
static void run_command(char *line)
{
    char target[200];
    char *p, *path = NULL;
    int append = 0, n;

    for (p = line; *p; p++) {
        if (*p == '>') {
            *p = 0;
            p++;
            if (*p == '>') {
                append = 1;
                p++;
            }
            while (*p == ' ')
                p++;
            if (*p)
                path = p;
            break;
        }
    }

    if (!path) {
        run_command_body(line);
        return;
    }

    strncpy(target, path, sizeof(target) - 1);
    target[sizeof(target) - 1] = 0;

    cap_len = 0;
    cap_full = 0;
    cap_on = 1;
    if (append) {
        n = fs_read(target, cap_buf, (u32)(CAP_MAX / 2));
        if (n > 0)
            cap_len = n;
    }
    run_command_body(line);
    cap_on = 0;

    n = fs_write(target, cap_buf, (u32)cap_len);
    if (n >= 0)
        term_printf("-> %s (%d bytes%s)\n", target, cap_len,
                    cap_full ? ", truncated" : "");
    else
        term_printf("-> %s: write failed\n", target);
}


static void term_draw(window_t *w)
{
    int y = w->cy + 1;
    for (int i = 0; i < TERM_ROWS; i++) {
        if (term.lines[i][0])
            gfx_text(w->cx + 2, y, term.lines[i], RGB(210, 225, 200), 1);
        y += 10;
    }

    int iy = w->cy + 1 + term.cy * 10;
    if (iy + 9 < w->cy + w->ch) {
        gfx_text(w->cx + 2 + 7 * 8, iy, term.inbuf, RGB(240, 240, 240), 1);
        int caret = w->cx + 2 + 7 * 8 + term.incur * 8;
        gfx_fill_rect(caret, iy, 7, 8, RGB(120, 200, 120));
    }
}

static void term_history(int dir)
{
    if (term.hist_count == 0)
        return;
    if (dir < 0) {
        if (term.hist_pos > 0)
            term.hist_pos--;
    } else {
        if (term.hist_pos < term.hist_count - 1)
            term.hist_pos++;
        else {
            term.hist_pos = term.hist_count;
            term.inbuf[0] = 0;
            term.inlen = 0;
            term.incur = 0;
            return;
        }
    }
    strncpy(term.inbuf, term.hist[term.hist_pos], TERM_COLS);
    term.inlen = (int)strlen(term.inbuf);
    term.incur = term.inlen;
}

#define TAB_MAX 16

typedef struct {
    const char *pfx;
    int  n;
    char names[TAB_MAX][40];
} tab_t;

static int ci_prefix(const char *s, const char *p)
{
    while (*p) {
        char a = *s, b = *p;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (!a || a != b)
            return 0;
        s++;
        p++;
    }
    return 1;
}

static int tab_cb(const fs_dirent_t *e, void *ctx)
{
    tab_t *t = (tab_t *)ctx;

    if (!e->name[0] || !ci_prefix(e->name, t->pfx))
        return 0;
    if (t->n < TAB_MAX) {
        strncpy(t->names[t->n], e->name, 39);
        t->names[t->n][39] = 0;
        t->n++;
    }
    return 0;
}

/* 把 inbuf 的 [wstart, incur) 换成 repl（只在光标位于行尾时调用） */
static void inbuf_replace(int wstart, const char *repl)
{
    int add = (int)strlen(repl);

    if (wstart + add > TERM_COLS - 1)
        add = TERM_COLS - 1 - wstart;
    if (add < 0)
        return;
    memcpy(term.inbuf + wstart, repl, (size_t)add);
    term.inbuf[wstart + add] = 0;
    term.inlen = wstart + add;
    term.incur = term.inlen;
}

static void tab_complete(void)
{
    int wstart = 0, i, wlen, dlen = 0, plen;
    char word[TERM_COLS + 1];
    char dirpart[160], namepfx[48], path[220], repl[220];
    tab_t t;

    if (term.incur != term.inlen || term.incur <= 0)
        return;

    for (i = term.incur - 1; i >= 0; i--) {
        if (term.inbuf[i] == ' ') {
            wstart = i + 1;
            break;
        }
    }
    wlen = term.incur - wstart;
    if (wlen <= 0 || wlen > TERM_COLS)
        return;
    memcpy(word, term.inbuf + wstart, (size_t)wlen);
    word[wlen] = 0;

    plen = wlen;
    for (i = wlen - 1; i >= 0; i--) {
        if (word[i] == '/') {
            dlen = i + 1;
            plen = wlen - dlen;
            break;
        }
    }
    if (dlen) {
        memcpy(dirpart, word, (size_t)dlen);
        dirpart[dlen] = 0;
    } else {
        dirpart[0] = 0;
    }
    memcpy(namepfx, word + dlen, (size_t)plen);
    namepfx[plen] = 0;

    fs_path(path, sizeof(path), cwd, dlen ? dirpart : ".");
    if (!fs_is_dir(path))
        return;

    memset(&t, 0, sizeof(t));
    t.pfx = namepfx;
    fs_dir_scan(path, tab_cb, &t);
    if (t.n == 0)
        return;

    if (t.n == 1) {
        snprintf(repl, sizeof(repl), "%s%s", dirpart, t.names[0]);
        fs_path(path, sizeof(path), cwd, repl);
        if (fs_is_dir(path)) {
            int l = (int)strlen(repl);
            if (l < (int)sizeof(repl) - 1) {
                repl[l] = '/';
                repl[l + 1] = 0;
            }
        }
        inbuf_replace(wstart, repl);
        return;
    }

    /* 多个候选：先补到公共前缀，再把候选列出来 */
    {
        char cp[40];
        int k = 0;

        strcpy(cp, t.names[0]);
        for (i = 1; i < t.n; i++) {
            while (cp[k] && t.names[i][k]) {
                char a = cp[k], b = t.names[i][k];
                if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                if (a != b)
                    break;
                k++;
            }
            cp[k] = 0;
        }
        if ((int)strlen(cp) > plen) {
            snprintf(repl, sizeof(repl), "%s%s", dirpart, cp);
            inbuf_replace(wstart, repl);
        }
    }

    term_putc('\n');
    for (i = 0; i < t.n; i++) {
        term_puts(t.names[i]);
        term_puts(i + 1 < t.n ? "   " : "");
        if ((i + 1) % 3 == 0 && i + 1 < t.n)
            term_putc('\n');
    }
    term_putc('\n');
    term_prompt();
    term_puts(term.inbuf);
}

static void term_key(window_t *w, int key, u32 mods)
{
    (void)w;
    (void)mods;

    if (key == KEY_UP) {
        term_history(-1);
    } else if (key == KEY_DOWN) {
        term_history(1);
    } else if (key == KEY_LEFT) {
        if (term.incur > 0)
            term.incur--;
    } else if (key == KEY_RIGHT) {
        if (term.incur < term.inlen)
            term.incur++;
    } else if (key == KEY_HOME) {
        term.incur = 0;
    } else if (key == KEY_END) {
        term.incur = term.inlen;
    } else if (key == KEY_DELETE) {
        if (term.incur < term.inlen) {
            for (int i = term.incur; i < term.inlen; i++)
                term.inbuf[i] = term.inbuf[i + 1];
            term.inlen--;
        }
    } else if (key == '\t') {
        tab_complete();
    } else if (key == '\n') {
        char cmd[TERM_COLS + 1];

        term.inbuf[term.inlen] = 0;
        strncpy(cmd, term.inbuf, TERM_COLS);
        cmd[TERM_COLS] = 0;
        term.inlen = 0;
        term.incur = 0;
        term.inbuf[0] = 0;

        term_puts(cmd);
        term_putc('\n');

        if (cmd[0]) {
            if (term.hist_count < HIST_MAX) {
                strcpy(term.hist[term.hist_count++], cmd);
            } else {
                for (int i = 1; i < HIST_MAX; i++)
                    strcpy(term.hist[i - 1], term.hist[i]);
                strcpy(term.hist[HIST_MAX - 1], cmd);
            }
            term.hist_pos = term.hist_count;
            run_command(cmd);
        }
        if (term.cx != 0)
            term_putc('\n');
        term_prompt();
    } else if (key == '\b') {
        if (term.incur > 0) {
            for (int i = term.incur - 1; i < term.inlen; i++)
                term.inbuf[i] = term.inbuf[i + 1];
            term.incur--;
            term.inlen--;
        }
    } else if (key >= 32 && key < 127) {
        if (term.inlen < TERM_COLS - 1) {
            for (int i = term.inlen; i > term.incur; i--)
                term.inbuf[i] = term.inbuf[i - 1];
            term.inbuf[term.incur] = (char)key;
            term.inlen++;
            term.incur++;
            term.inbuf[term.inlen] = 0;
        }
    }
    gui_dirty_window(win);
}

void term_external_write(const char *buf, u32 len)
{
    if (win && win != gui_focused())
        gui_open(&app_terminal);

    for (u32 i = 0; i < len; i++) {
        if (buf[i] == '\n')
            term_putc('\n');
        else
            term_putc(buf[i]);
    }
    gui_dirty_window(win);
}

static void term_mouse(window_t *w, int x, int y, int buttons, int event)
{
    (void)w;
    (void)x;
    (void)y;
    (void)buttons;
    (void)event;
}

static void term_init(window_t *w)
{
    (void)w;
    memset(&term, 0, sizeof(term));
    win = w;
    strcpy(cwd, "/");
    term_puts(L("term.welcome"));
    term_puts(L("term.mounted"));
    term_prompt();
#ifdef AUTOJS_AUTORUN
    /* 调试钩子：仅验证构建自动跑，正式镜像不带此宏 */
    term_puts("[autojs] (placeholder)\n");
#endif
}

static void term_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_terminal = {
    .title  = "Terminal",
    .width  = TERM_W,
    .height = TERM_H,
    .init   = term_init,
    .draw   = term_draw,
    .key    = term_key,
    .mouse  = term_mouse,
    .close  = term_close,
};

#ifdef SCHED_AUTORUN
/* 仅验证用：自动跑测试 */
void term_autorun(void)
{
#ifdef AUTOTEST_RUNNER
    term_puts("\n[autorun] spawn API: runner -> HELLO.APP\n");
    gui_dirty_window(win);
    run_program("/APPS/RUNNER.APP", "/APPS/HELLO.APP");
    return;
#endif
#ifdef AUTOTEST_G2048
    term_puts("\n[autorun] user-mode GUI app: 2048\n");
    gui_dirty_window(win);
    run_program("/APPS/G2048.APP", (const char *)0);
    return;
#endif
#if defined(AUTOTEST_TC) || defined(AUTOTEST_ALL)
    term_puts("\n[autorun] TinyC: compile FIB.T (expect exit 144)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/FIB.T /APPS/FIB.APP");
    term_puts("\n[autorun] TinyC: compile SIEVE.T (expect exit 25)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/SIEVE.T /APPS/SIEVE.APP");
    term_puts("\n[autorun] TinyC: compile PRINT.T (expect exit 42)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/PRINT.T /APPS/PRINT.APP");
    term_puts("\n[autorun] TinyC: compile PTR.T (expect exit 60)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/PTR.T /APPS/PTR.APP");
    term_puts("\n[autorun] TinyC: compile STRUCT.T (expect exit 15)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/STRUCT.T /APPS/STRUCT.APP");
    term_puts("\n[autorun] TinyC: compile STRUCT2.T (expect exit 66)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/STRUCT2.T /APPS/STRUCT2.APP");
    term_puts("\n[autorun] TinyC: compile STRUCT3.T (expect exit 49)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/STRUCT3.T /APPS/STRUCT3.APP");
    term_puts("\n[autorun] TinyC: compile STRUCT4.T (expect exit 60, linked list)\n");
    gui_dirty_window(win);
    run_program("/APPS/TC.APP", "/DOCS/STRUCT4.T /APPS/STRUCT4.APP");
#ifndef AUTOTEST_ALL
    return;
#endif
#endif
#if defined(AUTOTEST_SHELL) || defined(AUTOTEST_ALL)
    term_puts("\n[autorun] shell: redirect > file, cat, wc, head, tail\n");
    gui_dirty_window(win);
    {
        char c1[] = "echo shell redirect works > /DOCS/T1.TXT";
        char c2[] = "cat /DOCS/T1.TXT";
        char c3[] = "wc /DOCS/T1.TXT";
        char c4[] = "head 3 /DOCS/FIB.T";
        char c5[] = "tail 1 /DOCS/FIB.T";
        run_command(c1);
        run_command(c2);
        run_command(c3);
        run_command(c4);
        run_command(c5);
    }
#endif
#if defined(AUTOTEST_JVM) || defined(AUTOTEST_ALL)
    term_puts("\n[autorun] JVM: run /APPS/HELLO.CLASS\n");
    gui_dirty_window(win);
    run_program("/APPS/JAVM.APP", "/APPS/HELLO.CLASS -");
    term_puts("\n[autorun] JVM: run /APPS/OBJECTDEMO.CLASS\n");
    gui_dirty_window(win);
    run_program("/APPS/JAVM.APP", "/APPS/OBJECTDEMO.CLASS -");
    term_puts("\n[autorun] JVM: run /APPS/STRINGDEMO.CLASS\n");
    gui_dirty_window(win);
    run_program("/APPS/JAVM.APP", "/APPS/STRINGDEMO.CLASS -");
    return;
#endif
    term_puts("\n[autorun] javm: MinecraftServer...\n");
    gui_dirty_window(win);
    {
        u8 *buf = kmalloc(240000);
        if (buf) {
            int r1 = fs_read("/APPS/JAVM.APP", buf, 240000);
            int r2 = fs_read("/APPS/GL3D.APP", buf, 240000);
            int r3 = fs_read("/APPS/MCSERVER.JAR", buf, 240000);
            int c0 = fs_count("/");
            int ca = fs_count("/APPS");
            fs_dirent_t e0;
            int first = fs_list("/", 0, &e0);
            kprintf("[diag] c/=%d c/APPS=%d first=%d JAVM=%d GL3D=%d MCJAR=%d\n",
                    c0, ca, first, r1, r2, r3);
        }
    }
    /* AUTOTEST: 先跑 js 自测（分段验证），再跑 javm（同轮双验证） */
#ifdef AUTOTEST_JS
    run_program("/APPS/JS.APP", 0);
#endif
    run_program("/APPS/JAVM.APP", "net/minecraft/server/MinecraftServer /APPS/MCSERVER.JAR");
}
#endif
