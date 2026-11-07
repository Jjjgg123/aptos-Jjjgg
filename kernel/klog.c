#include "klog.h"
#include "serial.h"
#include "util.h"

static char lines[KLOG_LINES][KLOG_COLS];
static int  count;
static int  head;
static int  col;
static int  screen_on;
static int  vga_row;
static int  vga_col;

extern void console_pump(void);

void klog_init(void)
{
    memset(lines, 0, sizeof(lines));
    count = 0;
    head = 0;
    col = 0;
    screen_on = 0;
    vga_row = 0;
    vga_col = 0;
}

static void newline(void)
{
    lines[head][col] = 0;
    head = (head + 1) % KLOG_LINES;
    col = 0;
    memset(lines[head], 0, KLOG_COLS);
    if (count < KLOG_LINES)
        count++;
}

static void vga_mirror(char c)
{
    volatile u16 *vga = (volatile u16 *)0xB8000;

    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= 25)
            vga_row = 24;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c >= 32 && c < 127) {
        vga[vga_row * 80 + vga_col] = (u16)(0x0700 | (u8)c);
        if (++vga_col >= 80) {
            vga_col = 0;
            if (++vga_row >= 25)
                vga_row = 24;
        }
    }
}

static void emit(char c)
{
    serial_putc(c);

    if (!screen_on)
        vga_mirror(c);
    if (c == '\n') {
        newline();
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\t') {
        do {
            lines[head][col++] = ' ';
        } while (col < KLOG_COLS - 1 && (col & 7));
    } else {
        lines[head][col++] = c;
        if (col >= KLOG_COLS - 1)
            newline();
    }
    if (screen_on)
        console_pump();
}

void klog_screen(int enable)
{
    screen_on = enable;
}

void kvprintf(const char *fmt, va_list ap)
{
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    const char *p = buf;
    while (*p)
        emit(*p++);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

int klog_count(void)
{
    return count;
}

const char *klog_line(int index)
{
    if (index < 0 || index >= count)
        return "";
    int start = (count == KLOG_LINES) ? head : 0;
    return lines[(start + index) % KLOG_LINES];
}
