#include "aptos.h"

int main(void)
{
    char name[40];
    int t[6];
    int i;

    printf("\n");
    printf("hello from aptos user mode!\n");
    printf("this code is running in ring 3, linked at 0x40000000\n\n");

    if (sys_time(t) == 0)
        printf("rtc      : %04d-%02d-%02d %02d:%02d:%02d\n", t[0], t[1], t[2], t[3],
               t[4], t[5]);

    printf("list of /:\n");
    for (i = 0;; i++) {
        int r = sys_list("/", i, name, sizeof(name));
        if (r == -1)
            break;
        if (r == -3)
            printf("   [DIR]  %s\n", name);
        else
            printf("   %-20s %6d bytes\n", name, r);
    }

    printf("\nsyscalls used: write, time, list, exit\n");
    return 0;
}
