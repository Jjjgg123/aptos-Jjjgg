#ifndef APTOS_SERIAL_H
#define APTOS_SERIAL_H

#include "types.h"

#define COM1 0x3F8

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_has_data(void);
int  serial_getc(void);

#endif
