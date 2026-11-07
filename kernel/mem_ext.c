#include "mem_ext.h"
#include "bootinfo.h"
#include "klog.h"

/* 仅用于探测：不依赖 kmain 的 cpu_has_cpuid（那是静态函数） */
static int cpu_has_cpuid(void)
{
    u32 fl0, fl1;

    __asm__ volatile("pushfl\n\tpopl %0" : "=r"(fl0));
    fl1 = fl0 ^ (1u << 21);
    __asm__ volatile("pushl %0\n\tpopfl\n\tpushfl\n\tpopl %0" : "+r"(fl1));
    return ((fl0 ^ fl1) & (1u << 21)) != 0;
}

int mem_ext_cpu_pae(void)
{
    u32 max_id = 0, a = 0, b = 0, c = 0, d = 0;

    if (!cpu_has_cpuid())
        return 0;

    __asm__ volatile("cpuid" : "=a"(max_id) : "a"(0) : "ebx", "ecx", "edx");
    if (max_id < 1)
        return 0;

    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1));
    (void)a; (void)b; (void)c;

    return (d >> 6) & 1u;   /* EDX[6] = PAE */
}

u64 mem_ext_high_bytes(void)
{
    e820_entry_t *map = (e820_entry_t *)BOOTINFO->e820_addr;
    u64 total = 0;
    u32 i;

    for (i = 0; i < BOOTINFO->e820_count; i++) {
        u64 start, end;

        if (map[i].type != 1 || map[i].length == 0)
            continue;

        start = map[i].base;
        end = map[i].base + map[i].length;

        if (start >= 0x100000000ull) {
            total += map[i].length;
        } else if (end > 0x100000000ull) {
            total += end - 0x100000000ull;   /* 跨过 4 GiB 边界的那一段 */
        }
    }
    return total;
}

void mem_ext_report(void)
{
    u64 high = mem_ext_high_bytes();
    u32 a = 0, b = 0, c = 0, d = 0;

    if (cpu_has_cpuid())
        __asm__ volatile("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1));
    (void)a; (void)b; (void)c;

    kprintf("[mem-ext] cpuid.01 edx=0x%08x (bit6 PAE=%d)\n", d, (d >> 6) & 1);
    kprintf("[mem-ext] PAE capable: %s, RAM above 4 GiB: %u MiB, mode: %s\n",
            mem_ext_cpu_pae() ? "yes" : "no",
            (u32)(high >> 20),
            MEM_EXT_PAE ? "pae" : "off (default)");

    if (high && !MEM_EXT_PAE)
        kprintf("[mem-ext] %u MiB above 4 GiB is reported but unusable "
                "without PAE (build with -DMEM_EXT_PAE=1 to opt in)\n",
                (u32)(high >> 20));
}
