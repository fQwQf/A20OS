OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5 /* R-X */);
    rodata PT_LOAD FLAGS(4 /* R-- */);
    data   PT_LOAD FLAGS(6 /* RW- */);
}

SECTIONS {
    . = 0x80000000;

    .text : {
        *(.text.entry)
        *(.text .text.*)
        /* EENTRY / TLBRENTRY zero the low 6 address bits, so both exception
         * entry points must land on a 64-byte boundary.  Keep the trap code
         * inside the .text PT_LOAD so the flat image (objcopy -O binary)
         * carries it, and align it here because the la32 ld does not honor
         * input-section alignment. */
        . = ALIGN(64);
        *(.trapcode)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    . = ALIGN(8);
    .driver_init : {
        __driver_init_start = .;
        KEEP(*(.driver_init))
        __driver_init_end = .;
    } :data

    .board_init : {
        __board_init_start = .;
        KEEP(*(.board_init))
        __board_init_end = .;
    } :data

    . = ALIGN(4096);
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        *(.bss.stack)
        . = ALIGN(4096);
        _bss_end = .;
    } :data

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
