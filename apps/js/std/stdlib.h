#ifndef APTOS_STD_STDLIB_H
#define APTOS_STD_STDLIB_H

#include <stddef.h>

#define RAND_MAX 32767

void *malloc(size_t size);
void *calloc(size_t n, size_t sz);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
void  abort(void) __attribute__((noreturn));
void  exit(int status) __attribute__((noreturn));
int   atoi(const char *s);
long  strtol(const char *s, char **endp, int base);
int   rand(void);
void  srand(unsigned seed);
int   abs(int x);
long  labs(long x);
void  qsort(void *base, size_t n, size_t sz,
             int (*cmp)(const void *, const void *));

#endif
