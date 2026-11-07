BITS 32

global user_enter
global user_exit_asm
global user_exit_status

section .bss
align 4
user_kernel_esp:  resd 1
user_exit_status: resd 1

section .text

user_enter:
        mov     eax, [esp + 4]
        mov     edx, [esp + 8]
        mov     [user_kernel_esp], esp

        mov     cx, 0x23
        mov     ds, cx
        mov     es, cx
        mov     fs, cx
        mov     gs, cx

        push    dword 0x23
        push    edx
        pushfd
        or      dword [esp], 0x200
        push    dword 0x1B
        push    eax
        iret

user_exit_asm:
        mov     eax, [esp + 4]
        mov     [user_exit_status], eax
        mov     esp, [user_kernel_esp]
        sti
        ret
