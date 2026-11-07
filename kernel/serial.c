#include "serial.h"
#include "io.h"

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c)
{
    int guard = 100000;
    while (!(inb(COM1 + 5) & 0x20) && guard--)
        ;
    outb(COM1, (u8)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

int serial_has_data(void)
{
    return (inb(COM1 + 5) & 0x01) != 0;
}

int serial_getc(void)
{
    if (!serial_has_data())
        return -1;
    return (int)inb(COM1);
}
