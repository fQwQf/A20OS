/* a20.ld — A20 Native ELF linker script (architecture-neutral) */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
    bss    PT_LOAD FLAGS(6);   /* R+W */
    a20_native 0x70000001 FLAGS(0);
}

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.got .got.*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    } :bss

    . = ALIGN(0x1000);
    _end = .;

    .a20_info : { . += 0; } :a20_native

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame*)
    }
}
