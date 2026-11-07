OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    __text_start = .;
    .text : {
        *(.text.entry)
        *(.text)
        *(.text.*)
    }
    __text_end = .;

    __rodata_start = .;
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    __rodata_end = .;

    .data : {
        *(.data)
        *(.data.*)
    }

    __bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    __bss_end = .;

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
