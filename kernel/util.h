#ifndef APTOS_UTIL_H
#define APTOS_UTIL_H

#include <stdarg.h>
#include "types.h"

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
int    atoi(const char *s);
void   utoa(u32 v, char *buf, int base);
void   itoa(int v, char *buf, int base);

int    vsnprintf(char *out, size_t size, const char *fmt, va_list ap);
int    snprintf(char *out, size_t size, const char *fmt, ...);

unsigned long long __udivdi3(unsigned long long a, unsigned long long b);
unsigned long long __umoddi3(unsigned long long a, unsigned long long b);

#endif
