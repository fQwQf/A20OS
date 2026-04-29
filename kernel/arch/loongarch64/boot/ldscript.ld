OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R-X */
    rodata PT_LOAD FLAGS(4); /* R-- */
    data   PT_LOAD FLAGS(6); /* RW- */
}

SECTIONS {
    . = 0x80000000;

    .text : {
        *(.text.entry)
        *(.text .text.*)
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
