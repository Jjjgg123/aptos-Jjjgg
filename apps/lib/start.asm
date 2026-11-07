BITS 32

global _start
extern main

section .text.entry
_start:
        xor     ebp, ebp
        call    main
        mov     ebx, eax
        mov     eax, 0
        int     0x80
        hlt
