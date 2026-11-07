/* memtest: 验证用户态堆 sys_brk + malloc/free/calloc/realloc */
#include "aptos.h"

int main(void)
{
    printf("memtest: brk(0)=0x%x\n", (unsigned)sys_brk(0));

    void *a = malloc(100);
    void *b = malloc(1024);
    void *c = calloc(64, 2);
    printf("a=%p b=%p c=%p\n", a, b, c);

    char *p = (char *)a;
    for (int i = 0; i < 100; i++)
        p[i] = (char)(i & 0x7f);
    int ok = 1;
    for (int i = 0; i < 100; i++)
        if (p[i] != (char)(i & 0x7f))
            ok = 0;
    for (int i = 0; i < 128; i++)
        if (((char *)c)[i] != 0)
            ok = 0;
    printf("roundtrip %s\n", ok ? "OK" : "FAIL");

    free(a);
    free(b);
    free(c);
    void *d = malloc(200);
    printf("after free, malloc(200)=%p\n", d);

    void *e = realloc(d, 500);
    printf("realloc 200->500: %p\n", e);
    ok = 1;
    for (int i = 0; i < 500; i++)
        ((char *)e)[i] = 0x5A;
    for (int i = 0; i < 500; i++)
        if (((char *)e)[i] != 0x5A)
            ok = 0;
    printf("realloc roundtrip %s\n", ok ? "OK" : "FAIL");

    printf("memtest done, brk now 0x%x\n", (unsigned)sys_brk(0));
    return 0;
}
