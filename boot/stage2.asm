BITS 16
ORG 0x8000

%define BOOTINFO      0x5000
%define E820_ADDR     0x5200
%define VBE_CTRL      0x5600
%define VBE_MODE      0x5900
%define MODELIST      0x5C00
%define KERNEL_ADDR   0x10000
%define KERNEL_SEG    0x1000
%define SPT           18
%define HEADS         2
%define DEF_SPT       18
%define DEF_HEADS     2

start:
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        cld
        mov     [boot_drive], dl

        ; 读真实 CHS 几何（AH=08h），失败回退 18/2，避免写死几何读错扇区
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

        call    serial_init16
        mov     si, msg_boot
        call    puts16
        call    do_e820
        call    do_vbe
        call    do_a20
        call    load_kernel
        call    fill_bootinfo

        mov     si, msg_pm
        call    puts16
        cli
        lgdt    [gdt_ptr]
        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax
        jmp     0x08:pm_entry

do_e820:
        xor     ax, ax
        mov     es, ax
        mov     di, E820_ADDR
        xor     ebx, ebx
        xor     bp, bp
.loop:
        mov     eax, 0xE820
        mov     edx, 0x534D4150
        mov     ecx, 24
        mov     dword [di + 20], 1
        push    es
        push    di
        push    ds
        push    es
        int     0x15
        pop     es
        pop     ds
        pop     di
        pop     es
        jc      .done
        cmp     eax, 0x534D4150
        jne     .done
        mov     eax, [di + 8]
        or      eax, [di + 12]
        jz      .skip
        inc     bp
        add     di, 24
        cmp     bp, 40
        jae     .done
.skip:
        test    ebx, ebx
        jnz     .loop
.done:
        movzx   eax, bp
        test    eax, eax
        jnz     .store
        ; ---- E820 一项都没拿到：用 int 15h AH=88h 探测扩展内存 ----
        mov     ah, 0x88
        int     0x15
        jc      .fallback
        test    ax, ax
        jz      .fallback
        movzx   ecx, ax                 ; KB
        shl     ecx, 10                 ; -> bytes
        mov     dword [E820_ADDR + 0], 0x00100000
        mov     dword [E820_ADDR + 4], 0
        mov     dword [E820_ADDR + 8], ecx
        mov     dword [E820_ADDR + 12], 0
        mov     dword [E820_ADDR + 16], 1
        mov     eax, 1
        jmp     .store
.fallback:
        mov     dword [E820_ADDR + 0], 0x00100000
        mov     dword [E820_ADDR + 4], 0
        mov     dword [E820_ADDR + 8], 0x00F00000   ; 15 MiB
        mov     dword [E820_ADDR + 12], 0
        mov     dword [E820_ADDR + 16], 1
        mov     eax, 1
.store:
        push    ax
        xor     ax, ax
        mov     es, ax
        pop     ax
        mov     [es:BOOTINFO + 0x14], eax
        mov     dword [es:BOOTINFO + 0x10], E820_ADDR
        mov     si, msg_e820
        call    puts16
        ret

do_vbe:
        xor     ax, ax
        mov     es, ax
        mov     ax, 0x4F00
        mov     di, VBE_CTRL
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .fallback

        mov     si, [VBE_CTRL + 0x0E]
        test    si, si
        jz      .fallback
        mov     ax, [VBE_CTRL + 0x10]
        mov     fs, ax
        mov     di, MODELIST
        mov     cx, 256
.copy_modes:
        mov     ax, [fs:si]
        add     si, 2
        mov     [di], ax
        add     di, 2
        cmp     ax, 0xFFFF
        je      .scan
        loop    .copy_modes
.scan:
        mov     word [best_mode], 0xFFFF
        mov     dword [best_score], 0
        mov     si, MODELIST
.next:
        mov     ax, [si]
        add     si, 2
        cmp     ax, 0xFFFF
        je      .choose
        test    ax, ax
        jz      .next
        mov     [cur_mode], ax
        mov     cx, ax
        mov     ax, 0x4F01
        mov     di, VBE_MODE
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .next
        cmp     byte [VBE_MODE + 0x1B], 6
        jne     .next
        mov     dx, [VBE_MODE + 0x12]
        cmp     dx, 640
        jb      .next
        cmp     dx, 1024
        ja      .next
        mov     dx, [VBE_MODE + 0x14]
        cmp     dx, 480
        jb      .next
        cmp     dx, 768
        ja      .next

        movzx   eax, byte [VBE_MODE + 0x19]
        cmp     al, 32
        je      .rank32
        cmp     al, 24
        je      .rank24
        cmp     al, 16
        je      .rank16
        jmp     .next
.rank32:
        mov     ebx, 3
        jmp     .ranked
.rank24:
        mov     ebx, 2
        jmp     .ranked
.rank16:
        mov     ebx, 1
.ranked:
        shl     ebx, 16
        movzx   eax, word [VBE_MODE + 0x12]
        movzx   ecx, word [VBE_MODE + 0x14]
        imul    eax, ecx
        shr     eax, 6
        add     ebx, eax
        cmp     ebx, [best_score]
        jbe     .next
        mov     [best_score], ebx
        mov     ax, [cur_mode]
        mov     [best_mode], ax
        jmp     .next

.choose:
        mov     ax, [best_mode]
        cmp     ax, 0xFFFF
        je      .fallback
        mov     cx, ax
        mov     bx, ax
        or      bx, 0x4000
        mov     ax, 0x4F02
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .fallback
        mov     cx, [best_mode]
        mov     ax, 0x4F01
        mov     di, VBE_MODE
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .fallback
        xor     ax, ax
        mov     es, ax
        mov     eax, [VBE_MODE + 0x28]
        mov     [es:BOOTINFO + 0x04], eax
        mov     ax, [VBE_MODE + 0x10]
        mov     [es:BOOTINFO + 0x08], ax
        mov     ax, [VBE_MODE + 0x12]
        mov     [es:BOOTINFO + 0x0A], ax
        mov     ax, [VBE_MODE + 0x14]
        mov     [es:BOOTINFO + 0x0C], ax
        mov     al, [VBE_MODE + 0x19]
        mov     [es:BOOTINFO + 0x0E], al
        mov     si, msg_vbe
        call    puts16
        ret
.fallback:
        mov     ax, 0x13
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        xor     ax, ax
        mov     es, ax
        mov     dword [es:BOOTINFO + 0x04], 0xA0000
        mov     word  [es:BOOTINFO + 0x08], 320
        mov     word  [es:BOOTINFO + 0x0A], 320
        mov     word  [es:BOOTINFO + 0x0C], 200
        mov     byte  [es:BOOTINFO + 0x0E], 8
        mov     si, msg_vga
        call    puts16
        ret

do_a20:
        mov     ax, 0x2401
        push    ds
        push    es
        int     0x15
        pop     es
        pop     ds
        call    empty_8042
        mov     al, 0xD1
        out     0x64, al
        call    empty_8042
        mov     al, 0xDF
        out     0x60, al
        call    empty_8042
        in      al, 0x92
        or      al, 2
        and     al, 0xFE
        out     0x92, al
        ret

empty_8042:
        mov     cx, 0xFFFF
.wait:
        in      al, 0x64
        test    al, 2
        jz      .done
        loop    .wait
.done:
        ret

load_kernel:
        mov     si, msg_kern
        call    puts16
        xor     ax, ax
        mov     es, ax
        mov     eax, [kernel_lba]
        mov     [cur_lba], eax
        movzx   eax, word [kernel_sec]
        mov     [left], ax
        mov     word [dst_seg], KERNEL_SEG
        mov     byte [chs_mode], 0
        mov     byte [retries], 0
.loop:
        cmp     word [left], 0
        je      .done
        cmp     byte [chs_mode], 0
        jne     .chs
        mov     ax, 32
        cmp     [left], ax
        jae     .cnt
        mov     ax, [left]
.cnt:
        mov     [dap_count], ax
        mov     eax, [cur_lba]
        mov     [dap_lba], eax
        mov     ax, [dst_seg]
        mov     [dap_seg], ax
        mov     word [dap_off], 0
        mov     si, dap
        mov     dl, [boot_drive]
        mov     ah, 0x42
        push    ds
        push    es
        int     0x13
        pop     es
        pop     ds
        jnc     .ok
        xor     ah, ah
        mov     dl, [boot_drive]
        push    ds
        push    es
        int     0x13
        pop     es
        pop     ds
        inc     byte [retries]
        cmp     byte [retries], 2
        jbe     .loop
        mov     byte [chs_mode], 1
        mov     byte [retries], 0
        call    lba_to_chs
        jmp     .loop

.chs:
        mov     ax, [dst_seg]
        mov     es, ax
        xor     bx, bx
        mov     ax, 0x0201
        mov     cx, [chs_cs]
        mov     dx, [chs_dh]
        int     0x13
        jc      .err
        inc     cl
        mov     al, [spt]
        inc     al
        cmp     cl, al
        jb      .cst
        mov     cl, 1
        inc     dh
        mov     al, [heads]
        cmp     dh, al
        jb      .cst
        mov     dh, 0
        inc     ch
.cst:
        mov     [chs_cs], cx
        mov     [chs_dh], dx
        dec     word [left]
        inc     dword [cur_lba]
        add     word [dst_seg], 0x20
        jmp     .loop

.ok:
        mov     byte [retries], 0
        mov     al, '.'
        call    serial_putc
        movzx   eax, word [dap_count]
        sub     [left], ax
        add     [cur_lba], eax
        shl     eax, 5
        add     [dst_seg], ax
        jmp     .loop
.err:
        mov     si, msg_derr
        call    puts16
        cli
        hlt
.done:
        ret

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


fill_bootinfo:
        xor     ax, ax
        mov     es, ax
        mov     dword [es:BOOTINFO + 0x00], 0x544F4F42
        mov     eax, [kernel_lba]
        mov     [es:BOOTINFO + 0x18], eax
        movzx   eax, word [kernel_sec]
        mov     [es:BOOTINFO + 0x1C], eax
        mov     eax, [fs_lba]
        mov     [es:BOOTINFO + 0x20], eax
        movzx   eax, byte [boot_drive]
        mov     [es:BOOTINFO + 0x24], eax
        ret

serial_init16:
        mov     dx, 0x3F9
        xor     al, al
        out     dx, al
        mov     dx, 0x3FB
        mov     al, 0x80
        out     dx, al
        mov     dx, 0x3F8
        mov     al, 1
        out     dx, al
        mov     dx, 0x3F9
        xor     al, al
        out     dx, al
        mov     dx, 0x3FB
        mov     al, 3
        out     dx, al
        mov     dx, 0x3FA
        mov     al, 0xC7
        out     dx, al
        mov     dx, 0x3FC
        mov     al, 0x0B
        out     dx, al
        ret

puts16:
        lodsb
        test    al, al
        jz      .done
        call    putc16
        jmp     puts16
.done:
        ret

putc16:
        push    ax
        push    dx
        mov     ah, 0x0E
        mov     bh, 0
        mov     bl, 7
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        pop     dx
        pop     ax
        push    ax
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        mov     cx, 0xFFFF
.wait:
        in      al, dx
        test    al, 0x20
        jnz     .out
        loop    .wait
.out:
        pop     dx
        pop     ax
        ret

msg_boot  db "[boot] aptos stage2", 13, 10, 0
msg_e820  db "[boot] e820 memory map collected", 13, 10, 0
msg_vbe   db "[boot] VBE linear framebuffer ready", 13, 10, 0
msg_vga   db "[boot] VBE unavailable, using VGA mode 13h", 13, 10, 0
msg_kern  db "[boot] loading kernel", 13, 10, 0
msg_kl    db "KL=", 0
msg_lk    db "L=", 0
msg_pm    db "[boot] entering protected mode", 13, 10, 0
msg_derr  db "[boot] disk read error!", 13, 10, 0

put_hex_byte:
        push    ax
        push    bx
        mov     bh, al
        shr     al, 4
        and     al, 0x0F
        mov     bl, al
        mov     al, [hexdig + bx]
        call    serial_putc
        mov     al, bh
        and     al, 0x0F
        mov     bl, al
        mov     al, [hexdig + bx]
        call    serial_putc
        pop     bx
        pop     ax
        ret

put_hex16:
        push    ax
        push    bx
        push    cx
        mov     cl, 12
.hexl:
        mov     bx, ax
        shr     bx, cl
        and     bx, 0x000F
        mov     al, [hexdig + bx]
        call    serial_putc
        sub     cl, 4
        jns     .hexl
        pop     cx
        pop     bx
        pop     ax
        ret

put_mem4:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     eax, [si]
        mov     ecx, 28
.hexl32:
        mov     ebx, eax
        shr     ebx, cl
        and     ebx, 0x0F
        mov     al, [hexdig + ebx]
        call    serial_putc
        sub     ecx, 4
        jns     .hexl32
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

put_hex32:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ecx, 28
.hexl32:
        mov     ebx, eax
        shr     ebx, cl
        and     ebx, 0x0F
        mov     al, [hexdig + ebx]
        call    serial_putc
        sub     ecx, 4
        jns     .hexl32
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

serial_putc:
        push    ax
        push    cx
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        mov     cx, 0xFFFF
.sw:
        in      al, dx
        test    al, 0x20
        jnz     .ok
        loop    .sw
.ok:
        pop     dx
        pop     cx
        pop     ax
        ret

hexdig  db "0123456789ABCDEF"

cur_mode   dw 0
best_mode  dw 0xFFFF
best_score dd 0
boot_drive db 0
retries   db 0
chs_mode  db 0
left      dw 0
cur_lba   dd 0
chs_cs    dw 0
chs_dh    dw 0
dst_seg   dw KERNEL_SEG
spt       db DEF_SPT
heads     db DEF_HEADS
spt_heads dw DEF_SPT * DEF_HEADS

align 4
dap:
        db 0x10, 0
dap_count dw 32
dap_off   dw 0
dap_seg   dw KERNEL_SEG
dap_lba   dq 1

patch_marker:
        db 'MYO2'
kernel_lba  dd 9
kernel_sec  dw 64
fs_lba      dd 0

align 8
gdt_start:
        dq 0x0000000000000000
gdt_code:
        dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
gdt_data:
        dw 0xFFFF, 0x0000, 0x9200, 0x00CF
gdt_end:

gdt_ptr:
        dw gdt_end - gdt_start - 1
        dd gdt_start

BITS 32
pm_entry:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0x90000      ; 跳板栈搬到 0x90000，不再占用 MBR/实模式 0x7C00 栈区
        jmp     KERNEL_ADDR
