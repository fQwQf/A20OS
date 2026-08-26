/* a20-dynamic.ld — A20 Native ELF linker script for PT_INTERP probes.
 *
 * Same layout contract as a20-generic.ld, plus an INTERP program header
 * so --dynamic-linker actually materializes the interpreter segment
 * (the generic script's explicit PHDRS suppresses it otherwise).
 * Used by the dynamic-linking bring-up probes (08-runtime-status §8a).
 */
ENTRY(_start)

PHDRS {
    interp PT_INTERP;
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
    bss    PT_LOAD FLAGS(6);   /* R+W */
    a20_native 0x6a20a200 FLAGS(0);
}

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;

    .interp : { *(.interp) } :interp

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
