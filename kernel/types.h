#ifndef APTOS_TYPES_H
#define APTOS_TYPES_H

typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;
typedef signed long long    s64;

typedef u32                 size_t;
typedef s32                 ssize_t;

#define NULL ((void *)0)
#define true  1
#define false 0

#define PACKED __attribute__((packed))

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif
