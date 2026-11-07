#ifndef APTOS_KLOG_H
#define APTOS_KLOG_H

#include <stdarg.h>
#include "types.h"

#define KLOG_LINES 128
#define KLOG_COLS  100

void klog_init(void);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);

void klog_screen(int enable);

int         klog_count(void);
const char *klog_line(int index);

#endif
