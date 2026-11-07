BITS 16
ORG 0x7C00

%define STAGE2_SEG 0x0800
%define DEF_SPT   18
%define DEF_HEADS 2

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        cld
        sti
        mov     [boot_drive], dl
        mov     byte [chs_mode], 0

        ; 用 int13h AH=08h 读真实 CHS 几何（真实硬盘多为 63/16，不再写死 18/2）
        mov     dl, [boot_drive]
        mov     ah, 0x08
        int     0x13
        jc      .geo_def
        mov     al, cl
        and     al, 0x3F
        jz      .geo_def
        mov     [spt], al
        inc     dh
        cmp     dh, 1
        jb      .geo_def
        mov     [heads], dh
        movzx   ax, byte [spt]
        movzx   bx, byte [heads]
        imul    ax, bx
        mov     [spt_heads], ax
        jmp     .geo_done
.geo_def:
        mov     byte [spt], DEF_SPT
        mov     byte [heads], DEF_HEADS
        mov     word [spt_heads], DEF_SPT * DEF_HEADS
.geo_done:

        mov     ax, [stage2_sec]
        mov     [left], ax
        mov     eax, [stage2_lba]
        mov     [cur_lba], eax
        mov     di, STAGE2_SEG

.rd:
        cmp     byte [chs_mode], 0
        jne     .rd_chs

        mov     eax, [cur_lba]
        mov     [dap_lba], eax
        mov     [dap_seg], di
        mov     word [dap_off], 0
        mov     word [dap_count], 1
        mov     si, dap
        mov     dl, [boot_drive]
        mov     ah, 0x42
        int     0x13
        jnc     .next

        mov     byte [chs_mode], 1
        call    lba_to_chs

.rd_chs:
        mov     ax, 0x0201
        mov     es, di
        xor     bx, bx
        mov     cx, [chs_cs]
        mov     dx, [chs_dh]
        int     0x13
        jc      .err

        inc     cl
        mov     al, [spt]
        inc     al
        cmp     cl, al
        jb      .st
        mov     cl, 1
        inc     dh
        mov     al, [heads]
        cmp     dh, al
        jb      .st
        mov     dh, 0
        inc     ch
.st:
        mov     [chs_cs], cx
        mov     [chs_dh], dx

.next:
        add     di, 0x20
        dec     word [left]
        jz      .go
        inc     dword [cur_lba]
        jmp     .rd

.go:
        mov     dl, [boot_drive]
        jmp     STAGE2_SEG:0x0000

.err:
        mov     si, msg_err
        call    puts16
        cli
        hlt

lba_to_chs:
        mov     eax, [cur_lba]
        xor     edx, edx
        movzx   ecx, word [spt_heads]
        div     ecx
        mov     ch, al
        mov     eax, edx
        xor     edx, edx
        movzx   ecx, byte [spt]
        div     ecx
        mov     dh, al
        mov     cl, dl
        inc     cl
        mov     dl, [boot_drive]
        mov     [chs_cs], cx
        mov     [chs_dh], dx
        ret

puts16:
        lodsb
        test    al, al
        jz      .done
        mov     ah, 0x0E
        mov     bh, 0
        mov     bl, 7
        int     0x10
        jmp     puts16
.done:
        ret

msg_err    db "aptos: disk error", 13, 10, 0
boot_drive db 0
chs_mode   db 0
left       dw 0
cur_lba    dd 0
chs_cs     dw 0
chs_dh     dw 0
spt        db DEF_SPT
heads      db DEF_HEADS
spt_heads  dw DEF_SPT * DEF_HEADS

align 4
dap:
        db 0x10, 0
dap_count dw 1
dap_off   dw 0
dap_seg   dw STAGE2_SEG
dap_lba   dq 1

patch_marker:
        db 'MYO1'
stage2_lba  dd 1
stage2_sec  dw 8

        times 510 - ($ - $$) db 0
        dw 0xAA55
