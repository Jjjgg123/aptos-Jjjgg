BITS 32

global _start
global stack_bottom, stack_top, stack_guard
extern kmain
extern __bss_start
extern __bss_end

section .text.entry
_start:
        cli
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, stack_top

        ; ---- 打开 CPU 缓存 -------------------------------------------------
        ; 实模式引导结束后 CR0.CD=1、NW=1，且 MTRR 默认类型是 UC：整机内存
        ; 访问都不走 cache。GUI 每帧 3 MB 的 present、用户态 GL 的逐像素填充
        ; 全部变成 DRAM 往返，表现就是"卡"。
        ; 这里把默认内存类型设成 WB 并清掉 CD/NW（帧缓冲仍由页表 PCD 位挡住）。
        xor     eax, eax
        cpuid
        cmp     eax, 1
        jb      .no_mtrr
        mov     eax, 1
        cpuid
        test    edx, (1 << 12)          ; CPUID.01:EDX[12] = MTRR 支持
        jz      .no_mtrr
        mov     ecx, 0x2FF              ; IA32_MTRR_DEF_TYPE
        rdmsr
        and     eax, 0xFFFFF7FF         ; 清 E（MTRR 全部关闭）
        or      eax, 0x6                ; 默认类型 = WB
        wrmsr
.no_mtrr:
        fninit                          ; 初始化 x87（CR0.EM 复位值已是 0）
        mov     eax, cr0
        and     eax, 0x9FFFFFFF         ; 清 CD(bit30) / NW(bit29)
        or      eax, 0x2                ; MP=1；NE 保持复位值 0，避免无 #MF 处理
        mov     cr0, eax

        mov     edi, __bss_start
        mov     ecx, __bss_end
        sub     ecx, edi
        xor     eax, eax
        rep     stosb

        call    kmain

.hang:
        cli
        hlt
        jmp     .hang

section .bss
ALIGNB 4096
stack_guard:
        resb    4096
stack_bottom:
        resb    65536
stack_top:
