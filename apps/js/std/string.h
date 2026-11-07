#ifndef APTOS_STD_STRING_H
#define APTOS_STD_STRING_H

#include <stddef.h>

/* aptos.h 提供 memset/memcpy/strlen/strcmp/strncmp/strcpy/strncpy */
#include "aptos.h"

void *memmove(void *d, const void *s, unsigned n);
int   memcmp(const void *a, const void *b, size_t n);
char *strchr(const char *s, int c);
char *strstr(const char *h, const char *n);

#endif
