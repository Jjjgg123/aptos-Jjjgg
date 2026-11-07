#ifndef APTOS_STD_SETJMP_H
#define APTOS_STD_SETJMP_H

/* x86-32: ebx, esi, edi, ebp, esp, eip, + 2 slack */
typedef unsigned long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
