#ifndef APTOS_MEM_EXT_H
#define APTOS_MEM_EXT_H

#include "types.h"

/* ---------------------------------------------------------------------
   内存拓展（可选特性，默认关闭）

   32 位内核的默认地址模型是"身份映射 3 GiB"，物理内存到 3 GiB 就封顶。
   想跳出 4 GiB 物理上限，必须走 PAE（Physical Address Extension）：
   页表项变成 8 字节、CR3 指向 PDPT、物理地址位宽提到 52 位。

   这条路线会同时改 boot/entry.asm、paging.c 和 E820 的取舍，风险高，
   所以做成编译期开关：默认 0 —— 保持原来的 32 位页表，行为完全不变；
   打开后（-DMEM_EXT_PAE=1）才启用 PAE 路径。

   未打开时仍然会探测并报告：CPU 是否支持 PAE、E820 里有多少 4 GiB
   以上的可用内存，方便判断这台机器值不值得开。
   --------------------------------------------------------------------- */
#ifndef MEM_EXT_PAE
#define MEM_EXT_PAE 0
#endif

/* CPU 是否支持 PAE（CPUID.01H:EDX[6]）；无 CPUID 的 386 级 CPU 返回 0 */
int  mem_ext_cpu_pae(void);

/* E820 中 4 GiB 以上、类型为可用（type==1）的字节数 */
u64  mem_ext_high_bytes(void);

/* 启动日志：PAE 能力 / 高位内存 / 当前开关状态 */
void mem_ext_report(void);

#endif
