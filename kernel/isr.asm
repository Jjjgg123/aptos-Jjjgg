BITS 32

section .text

%macro ISR_NOERR 1
global isr%1
isr%1:
        push    dword 0
        push    dword %1
        jmp     isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
        push    dword %1
        jmp     isr_common
%endmacro

%macro IRQ_STUB 1
global isr%1
isr%1:
        push    dword 0
        push    dword %1
        jmp     isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

%assign i 32
%rep 16
IRQ_STUB i
%assign i i+1
%endrep

extern isr_handler

isr_common:
        pusha
        push    ds
        push    es
        push    fs
        push    gs
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        push    esp
        call    isr_handler
        add     esp, 4
        ; isr_handler 返回新任务内核栈指针（eax），非 0 则切换
        test    eax, eax
        jz      .noswitch
        mov     esp, eax
.noswitch:
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popa
        add     esp, 8
        iret

global isr128
isr128:
        push    dword 0
        push    dword 0x80
        jmp     isr_common

section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
        dd isr%+i
%assign i i+1
%endrep
