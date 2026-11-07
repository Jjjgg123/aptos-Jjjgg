#include "aptos.h"

/* runner.app — 用户态进程启动器（验证 SYS_SPAWN/SYS_WAIT/SYS_PROCINFO）
 *
 * 用法：run /APPS/RUNNER.APP /APPS/HELLO.APP [子进程参数]
 * 行为：读参数 → spawn → 打印子进程信息 → wait → 打印退出码
 */

int main(void)
{
    char path[160];
    char arg0[160];
    char info[40];
    int pid, st;

    path[0] = 0;
    arg0[0] = 0;
    sys_args(0, path, sizeof(path));
    if (!path[0])
        sys_args(1, path, sizeof(path));   /* 终端单参数约定会把词放进 arg1 */
    if (!path[0]) {
        printf("runner: usage: run /APPS/RUNNER.APP <path.APP> [arg]\n");
        return 1;
    }
    sys_args(1, arg0, sizeof(arg0));
    if (!arg0[0] || !strcmp(arg0, path))
        arg0[0] = 0;

    printf("runner: self pid=%d\n", sys_getpid());
    if (arg0[0])
        printf("runner: spawn '%s' (arg '%s')\n", path, arg0);
    else
        printf("runner: spawn '%s'\n", path);

    pid = sys_spawn(path, arg0[0] ? arg0 : (const char *)0);
    if (pid < 0) {
        printf("runner: spawn failed (%d)\n", pid);
        return 2;
    }
    printf("runner: child pid=%d\n", pid);

    if (sys_procinfo(pid, info) == 0)
        printf("runner: procinfo name='%s'\n", info);

    st = sys_wait(pid);
    printf("runner: child %d exited with %d\n", pid, st);
    return 0;
}
