#include "aptos.h"

/* demo: 多进程抢占调度验证程序
   用法: run demo [name] [loops] 或 demo name loops &
   循环打印并睡眠，证明多个进程在时间片调度下并发交替运行 */

int main(void)
{
    char a0[80], a1[80];
    int loops = 10;
    int i;
    char name[48] = "demo";

    sys_args(0, a0, sizeof(a0));
    sys_args(1, a1, sizeof(a1));
    if (a0[0])
        strncpy(name, a0, sizeof(name) - 1);
    if (a1[0])
        loops = atoi(a1);
    if (loops < 1)
        loops = 1;
    if (loops > 200)
        loops = 200;

    for (i = 0; i < loops; i++) {
        printf("[%s] loop %d (uptime %d ms)\n", name, i, sys_ticks() * 10);
        sys_sleep(200);
    }
    printf("[%s] done after %d loops\n", name, i);
    return 42;
}
