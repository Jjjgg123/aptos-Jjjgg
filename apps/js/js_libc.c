/* aptos 用户态补充 libc：stdlib/string 里 uapp lib 未提供的部分 */
#include "std/stdlib.h"
#include "std/string.h"
#include "std/stdio.h"
#include "aptos.h"

/* sprintf：包装 vsnprintf（DUK_SPRINTF 用于 Date/Symbol 字符串） */
int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, 512, fmt, ap);
    va_end(ap);
    return n;
}

/* sscanf 最小实现：%p（JSON pointer）/ %d / %x，足够 Duktape 使用 */
static const char *s_skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    s = s_skip_ws(s);
    while (*fmt) {
        if (*fmt == ' ') {
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*fmt != *s)
                break;
            fmt++;
            s++;
            continue;
        }
        fmt++;
        if (*fmt == 'p') {
            void **pp = va_arg(ap, void **);
            unsigned long v = 0;
            int any = 0;
            s = s_skip_ws(s);
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                s += 2;
                for (;; s++) {
                    int c = (unsigned char)*s, dg;
                    if (c >= '0' && c <= '9')
                        dg = c - '0';
                    else if (c >= 'a' && c <= 'f')
                        dg = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F')
                        dg = c - 'A' + 10;
                    else
                        break;
                    v = v * 16 + (unsigned)dg;
                    any = 1;
                }
            } else if (*s == '(') {  /* (nil) */
                if (s[1] == 'n') {
                    v = 0;
                    any = 1;
                    s += 5;
                }
            }
            *pp = (void *)v;
            if (any)
                n++;
            fmt++;
        } else if (*fmt == 'd' || *fmt == 'i') {
            int *ip = va_arg(ap, int *);
            long v = 0;
            int neg = 0, any = 0;
            s = s_skip_ws(s);
            if (*s == '-') {
                neg = 1;
                s++;
            } else if (*s == '+')
                s++;
            while (*s >= '0' && *s <= '9') {
                v = v * 10 + (*s - '0');
                s++;
                any = 1;
            }
            *ip = neg ? (int)-v : (int)v;
            if (any)
                n++;
            fmt++;
        } else if (*fmt == 'x') {
            unsigned *up = va_arg(ap, unsigned *);
            unsigned v = 0;
            int any = 0;
            s = s_skip_ws(s);
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                s += 2;
            for (;; s++) {
                int c = (unsigned char)*s, dg;
                if (c >= '0' && c <= '9')
                    dg = c - '0';
                else if (c >= 'a' && c <= 'f')
                    dg = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    dg = c - 'A' + 10;
                else
                    break;
                v = v * 16 + (unsigned)dg;
                any = 1;
            }
            *up = v;
            if (any)
                n++;
            fmt++;
        } else {
            break;
        }
    }
    va_end(ap);
    return n;
}

void abort(void)
{
    for (;;) ;  /* 内核会看门狗？保守死循环 */
}

void exit(int status)
{
    sys_exit(status);
}

long strtol(const char *s, char **endp, int base)
{
    long v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    int any = 0;
    for (;; s++) {
        int c = (unsigned char)*s;
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * base + d;
        any = 1;
    }
    if (endp)
        *endp = (char *)(any ? s : (const char *)(neg ? s - 1 : s));
    return neg ? -v : v;
}

static unsigned long s_rand_state = 1;
int rand(void)
{
    s_rand_state = s_rand_state * 1103515245 + 12345;
    return (int)((s_rand_state >> 16) & 0x7FFF);
}

void srand(unsigned seed)
{
    s_rand_state = seed ? seed : 1;
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

/* 插入排序版 qsort（Duktape 的 qsort 主要用于少量排序，性能足够） */
void qsort(void *base, size_t n, size_t sz,
           int (*cmp)(const void *, const void *))
{
    char *b = (char *)base;
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        /* 逐字节交换 */
        while (j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0) {
            for (size_t k = 0; k < sz; k++) {
                char t = b[(j - 1) * sz + k];
                b[(j - 1) * sz + k] = b[j * sz + k];
                b[j * sz + k] = t;
            }
            j--;
        }
    }
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (*s == 0)
            return 0;
    }
}

char *strstr(const char *h, const char *n)
{
    if (!*n)
        return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return (char *)h;
    }
    return 0;
}
