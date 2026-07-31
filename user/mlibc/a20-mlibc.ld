/* a20-mlibc.ld — A20 Native ELF linker script for mlibc static binaries.
 *
 * Like a20-generic.ld but provides everything a static mlibc executable
 * needs: init/fini arrays with boundary symbols, TLS template sections
 * with a PT_TLS phdr, and .eh_frame retention.
 */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
    tls    PT_TLS  FLAGS(4);   /* R   */
    a20_native 0x6a20a200 FLAGS(0);
}

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;
    PROVIDE_HIDDEN(__ehdr_start = 0x400000);

    .text : {
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :rodata

    .tdata : {
        *(.tdata .tdata.*)
    } :rodata :tls

    . = ALIGN(0x1000);

    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array .preinit_array.*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } :data

    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array .init_array.*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data

    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array .fini_array.*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data

    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } :data

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.got .got.*)
    } :data

    .eh_frame : {
        *(.eh_frame .eh_frame.*)
    } :data

    .tbss : {
        *(.tbss .tbss.*)
    } :data :tls

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    . = ALIGN(0x1000);
    _end = .;

    .a20_info : { . += 0; } :a20_native

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
    }
}
