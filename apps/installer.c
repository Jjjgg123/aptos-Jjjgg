#include "aptos.h"

int main(void)
{
    char pkg[160];
    char name[40];
    char dest[200];
    int h, n, i, ok = 0, bytes = 0;

    printf("\n");
    printf("=== aptos package installer ===\n");

    if (sys_args(0, pkg, sizeof(pkg)) != 0) {
        printf("no package given\n");
        return 1;
    }
    printf("package : %s\n", pkg);

    h = sys_pkg_open(pkg);
    if (h < 0) {
        printf("cannot open package (error %d)\n", h);
        return 2;
    }
    n = sys_pkg_count(h);
    printf("entries : %d\n\n", n);

    if (!sys_exists("/APPS")) {
        printf("creating /APPS ...\n");
        if (sys_mkdir("/APPS") != 0) {
            printf("cannot create /APPS\n");
            return 3;
        }
    }

    for (i = 0; i < n; i++) {
        int size = sys_pkg_info(h, i, name);
        int r;

        if (size < 0) {
            printf("  entry %d: unreadable\n", i);
            continue;
        }
        snprintf(dest, sizeof(dest), "/APPS/%s.APP", name);
        r = sys_pkg_extract(h, i, dest);
        if (r < 0) {
            printf("  %-20s FAILED (%d)\n", name, r);
        } else {
            printf("  %-20s %6d bytes  -> %s\n", name, r, dest);
            ok++;
            bytes += r;
        }
    }

    printf("\n%d of %d files installed (%d bytes).\n", ok, n, bytes);
    printf("they are now on the desktop.\n");
    return 0;
}
