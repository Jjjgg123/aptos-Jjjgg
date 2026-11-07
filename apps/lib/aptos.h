#ifndef APTOS_APP_H
#define APTOS_APP_H

#include <stdarg.h>

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READKEY     2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_READ        5
#define SYS_MKDIR       6
#define SYS_UNLINK      7
#define SYS_LIST        8
#define SYS_SLEEP       9
#define SYS_TIME        10
#define SYS_GETKEY      11
#define SYS_ARGS        12
#define SYS_PKG_OPEN    13
#define SYS_PKG_COUNT   14
#define SYS_PKG_INFO    15
#define SYS_PKG_EXTRACT 16
#define SYS_EXISTS      17
#define SYS_ISDIR       18
#define SYS_WIN_OPEN    20
#define SYS_WIN_CLEAR   21
#define SYS_GFX_RECT    22
#define SYS_GFX_TEXT    23
#define SYS_WIN_FLUSH   24
#define SYS_MOUSE       25
#define SYS_WIN_FRAME   26
#define SYS_WIN_SIZE    27
#define SYS_PUMP        28
#define SYS_TICKS       29
#define SYS_POWEROFF    30
#define SYS_BOOTMODE    31
#define SYS_RAWWRITE    32
#define SYS_INSTALL     33
#define SYS_REBOOT      34
#define SYS_DISK_INFO   35
#define SYS_INSTALL_FAT 36
#define SYS_BRK         37
#define SYS_FILEWRITE   38
#define SYS_GFX_LINE    39
#define SYS_BEEP        40
#define SYS_SYSINFO     41
#define SYS_NET_INFO     42
#define SYS_TCP_CONNECT  43
#define SYS_TCP_SEND     44
#define SYS_TCP_RECV     45
#define SYS_TCP_CLOSE    46
#define SYS_DNS_QUERY    47
#define SYS_TCP_LISTEN   48
#define SYS_TCP_ACCEPT   49
#define SYS_GFX_BLIT     50
#define SYS_SPAWN        51
#define SYS_WAIT         52
#define SYS_KILL         53
#define SYS_GETPID       54
#define SYS_PROCINFO     55
#define SYS_MMAP         56
#define SYS_MUNMAP       57
#define SYS_WIN_EVENT    58
#define SYS_PCI          59
#define SYS_PRINTN       60
#define SYS_PCI_BAR      61
#define SYS_NET_SETIP    62
#define SYS_SERIAL_GETC  63

#define MOUSE_BTN_LEFT  1
#define MOUSE_BTN_RIGHT 2

static inline int __syscall(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
                     : "memory", "cc");
    return r;
}

static inline int __syscall5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory", "cc");
    return r;
}

static inline void sys_exit(int code)
{
    __syscall(SYS_EXIT, code, 0, 0, 0);
    for (;;)
        ;
}

static inline int sys_write(const char *s, int len)
{
    return __syscall(SYS_WRITE, 1, (int)s, len, 0);
}

static inline int sys_writefd(int fd, const void *buf, int len)
{
    return __syscall(SYS_WRITE, fd, (int)buf, len, 0);
}

static inline int sys_open(const char *path, int mode)
{
    return __syscall(SYS_OPEN, (int)path, mode, 0, 0);
}

static inline int sys_close(int fd) { return __syscall(SYS_CLOSE, fd, 0, 0, 0); }

static inline int sys_read(int fd, void *buf, int len)
{
    return __syscall(SYS_READ, fd, (int)buf, len, 0);
}

static inline int sys_mkdir(const char *path)
{
    return __syscall(SYS_MKDIR, (int)path, 0, 0, 0);
}

static inline int sys_unlink(const char *path)
{
    return __syscall(SYS_UNLINK, (int)path, 0, 0, 0);
}

static inline int sys_list(const char *path, int index, char *buf, int bufsize)
{
    return __syscall(SYS_LIST, (int)path, index, (int)buf, bufsize);
}

static inline void sys_sleep(int ms) { __syscall(SYS_SLEEP, ms, 0, 0, 0); }

static inline int sys_time(int *t) { return __syscall(SYS_TIME, (int)t, 0, 0, 0); }

static inline int sys_getkey(void) { return __syscall(SYS_GETKEY, 0, 0, 0, 0); }

static inline int sys_readkey(void) { return __syscall(SYS_READKEY, 0, 0, 0, 0); }

static inline int sys_args(int index, char *buf, int bufsize)
{
    return __syscall(SYS_ARGS, index, (int)buf, bufsize, 0);
}

static inline int sys_pkg_open(const char *path)
{
    return __syscall(SYS_PKG_OPEN, (int)path, 0, 0, 0);
}

static inline int sys_pkg_count(int h) { return __syscall(SYS_PKG_COUNT, h, 0, 0, 0); }

static inline int sys_pkg_info(int h, int index, char *name)
{
    return __syscall(SYS_PKG_INFO, h, index, (int)name, 32);
}



typedef struct {
    unsigned magic;
    unsigned ip;
    unsigned gw;
    unsigned dns;
    unsigned mask;
    unsigned char mac[6];
    unsigned char up;
} netinfo_t;

static inline int sys_net_info(netinfo_t *ni)
{
    return __syscall(SYS_NET_INFO, (int)ni, 0, 0, 0);
}

static inline int sys_net_setip(unsigned ip, unsigned gw, unsigned mask)
{
    return __syscall(SYS_NET_SETIP, (int)ip, (int)gw, (int)mask, 0);
}

static inline int sys_serial_getc(void)
{
    return __syscall(SYS_SERIAL_GETC, 0, 0, 0, 0);
}

static inline int sys_tcp_connect(const char *ip, int port)
{
    return __syscall(SYS_TCP_CONNECT, (int)ip, port, 0, 0);
}

static inline int sys_tcp_send(const void *buf, int len)
{
    return __syscall(SYS_TCP_SEND, 0, (int)buf, len, 0);
}

static inline int sys_tcp_recv(void *buf, int len, int timeout_ms)
{
    return __syscall(SYS_TCP_RECV, 0, (int)buf, len, timeout_ms);
}

static inline int sys_tcp_close(void)
{
    return __syscall(SYS_TCP_CLOSE, 0, 0, 0, 0);
}

static inline int sys_tcp_listen(int port)
{
    return __syscall(SYS_TCP_LISTEN, port, 0, 0, 0);
}

static inline int sys_tcp_accept(void)
{
    return __syscall(SYS_TCP_ACCEPT, 0, 0, 0, 0);
}

static inline int sys_dns_query(const char *name, u32 *ip)
{
    return __syscall(SYS_DNS_QUERY, (int)name, (int)ip, 0, 0);
}

static inline int sys_pkg_extract(int h, int index, const char *dest)
{
    return __syscall(SYS_PKG_EXTRACT, h, index, (int)dest, 0);
}

static inline int sys_exists(const char *path)
{
    return __syscall(SYS_EXISTS, (int)path, 0, 0, 0);
}

static inline int sys_isdir(const char *path)
{
    return __syscall(SYS_ISDIR, (int)path, 0, 0, 0);
}

static inline int sys_win_open(const char *title, int w, int h)
{
    return __syscall(SYS_WIN_OPEN, (int)title, w, h, 0);
}

static inline void sys_clear(int color) { __syscall(SYS_WIN_CLEAR, color, 0, 0, 0); }

static inline void sys_rect(int x, int y, int w, int h, int color)
{
    __syscall5(SYS_GFX_RECT, x, y, w, h, color);
}

static inline void sys_frame(int x, int y, int w, int h, int color)
{
    __syscall5(SYS_WIN_FRAME, x, y, w, h, color);
}

static inline void sys_text(int x, int y, const char *s, int color, int scale)
{
    __syscall5(SYS_GFX_TEXT, x, y, (int)s, color, scale);
}

static inline void sys_flush(void) { __syscall(SYS_WIN_FLUSH, 0, 0, 0, 0); }

static inline void sys_mouse(int *x, int *y, int *buttons, int *inside)
{
    __syscall(SYS_MOUSE, (int)x, (int)y, (int)buttons, (int)inside);
}

static inline int sys_win_size(void) { return __syscall(SYS_WIN_SIZE, 0, 0, 0, 0); }

static inline void sys_pump(void) { __syscall(SYS_PUMP, 0, 0, 0, 0); }

static inline int sys_ticks(void) { return __syscall(SYS_TICKS, 0, 0, 0, 0); }



typedef struct {
    unsigned magic;
    char os_name[32];
    char os_ver[32];
    char cpu_vendor[16];
    char cpu_model[48];
    unsigned cpu_feat;
    unsigned mem_total_kb;
    unsigned mem_free_kb;
    unsigned heap_used;
    unsigned uptime_ms;
    unsigned dev_count;
    char devs[8][40];
    unsigned mount_count;
    struct { char point[32]; char dev[8]; int type; char label[32];
             unsigned total_kb; unsigned free_kb; int writable; } mounts[8];
    unsigned proc_active;
    unsigned proc_status;
} sysinfo_t;

static inline int sys_sysinfo(sysinfo_t *si)
{
    return __syscall(SYS_SYSINFO, (int)si, 0, 0, 0);
}

static inline void sys_beep(int freq, int ms)
{
    __syscall(SYS_BEEP, freq, ms, 0, 0);
}

static inline void sys_line(int x0, int y0, int x1, int y1, unsigned color)
{
    __syscall5(SYS_GFX_LINE, x0, y0, x1, y1, (int)color);
}
static inline void sys_blit(int dx, int dy, int w, int h, const unsigned *src)
{
    __syscall5(SYS_GFX_BLIT, dx, dy, w, h, (int)src);
}
static inline int sys_filewrite(int fd, const void *data, int len)
{
    return __syscall(SYS_FILEWRITE, fd, (int)data, len, 0);
}
static inline int sys_brk(int addr) { return __syscall(SYS_BRK, addr, 0, 0, 0); }

/* ---- 进程管理（内核 kapi / syscall 51-55） ---- */
static inline int sys_spawn(const char *path, const char *arg0)
{
    return __syscall(SYS_SPAWN, (int)path, (int)arg0, 0, 0);
}
static inline int sys_wait(int pid) { return __syscall(SYS_WAIT, pid, 0, 0, 0); }
static inline int sys_kill(int pid) { return __syscall(SYS_KILL, pid, 0, 0, 0); }
static inline int sys_getpid(void) { return __syscall(SYS_GETPID, 0, 0, 0, 0); }
static inline int sys_procinfo(int pid, char *buf)
{
    return __syscall(SYS_PROCINFO, pid, (int)buf, 0, 0);
}

static inline void sys_poweroff(void) { __syscall(SYS_POWEROFF, 0, 0, 0, 0); }

static inline int sys_bootmode(void) { return __syscall(SYS_BOOTMODE, 0, 0, 0, 0); }

static inline int sys_install(const char *dev, int base)
{
    return __syscall(SYS_INSTALL, (int)dev, base, 0, 0);
}

static inline int sys_install_fat(const char *dev, int base, int type)
{
    return __syscall(SYS_INSTALL_FAT, (int)dev, base, type, 0);
}

static inline void sys_reboot(void) { __syscall(SYS_REBOOT, 0, 0, 0, 0); }

static inline int sys_disk_info(int index, char *name, int *sectors, int *type)
{
    return __syscall(SYS_DISK_INFO, index, (int)name, (int)sectors, (int)type);
}

static inline int sys_rawwrite(const char *dev, int lba, const void *buf, int count)
{
    return __syscall(SYS_RAWWRITE, (int)dev, lba, count, (int)buf);
}

int  printf(const char *fmt, ...);
int  puts(const char *s);
int  putchar(int c);
int  snprintf(char *out, int size, const char *fmt, ...);
int  vsnprintf(char *out, int size, const char *fmt, va_list ap);

unsigned strlen(const char *s);
int  strcmp(const char *a, const char *b);
int  strncmp(const char *a, const char *b, unsigned n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, unsigned n);
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int  atoi(const char *s);
void sys_sleep_ms(int ms);

#define RGB(r, g, b) ((int)(((r) << 16) | ((g) << 8) | (b)))

#endif
