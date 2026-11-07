; x86-32 setjmp/longjmp for aptos user mode (Duktape exception handling)
; jmp_buf layout (must match stub setjmp.h jmp_buf[6] = 24 bytes):
; [0]=ebx [4]=esi [8]=edi [12]=ebp [16]=esp [20]=eip
; NOTE: do NOT write past 24 bytes - duktape's duk_jmpbuf is exactly jmp_buf (24B)
;       and over-writing corrupts caller stack locals.

global setjmp
global longjmp

section .text

setjmp:
    mov     eax, [esp + 4]      ; jmp_buf
    mov     [eax + 0], ebx
    mov     [eax + 4], esi
    mov     [eax + 8], edi
    mov     [eax + 12], ebp
    mov     ecx, esp
    add     ecx, 4              ; saved esp = esp + 4 (pointing at return addr)
    mov     [eax + 16], ecx
    mov     ecx, [esp]          ; return address (eip)
    mov     [eax + 20], ecx
    xor     eax, eax            ; return 0
    ret

longjmp:
    mov     eax, [esp + 4]      ; jmp_buf
    mov     ecx, [esp + 8]      ; val
    mov     edx, [eax + 0]
    mov     ebx, edx
    mov     edx, [eax + 4]
    mov     esi, edx
    mov     edx, [eax + 8]
    mov     edi, edx
    mov     edx, [eax + 12]
    mov     ebp, edx
    mov     edx, [eax + 16]
    mov     esp, edx
    mov     edx, [eax + 20]
    test    ecx, ecx
    jnz     .have_val
    mov     ecx, 1
.have_val:
    mov     eax, ecx            ; return val (nonzero)
    jmp     edx                 ; jump to saved eip
