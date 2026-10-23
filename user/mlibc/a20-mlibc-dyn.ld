/* a20-mlibc-dyn.ld — A20 Native PIE executable linked against mlibc.so.
 *
 * Unlike a20-mlibc.ld this image is ET_DYN and carries PT_INTERP plus
 * PT_DYNAMIC/PT_PHDR.  The program headers describe four loadable segments
 * (headers+interp, text, rodata, data) so ld.so can map each with its final
 * permissions without an RW->RX mprotect that the native capability ceiling
 * would refuse.  0x6a20a200 stays the Native-ABI marker.
 */
ENTRY(_start)

PHDRS {
    phdr        PT_PHDR PHDRS;
    interp_load PT_LOAD FILEHDR PHDRS FLAGS(4);
    interp      PT_INTERP;
    text        PT_LOAD FLAGS(5);
    rodata      PT_LOAD FLAGS(4);
    data        PT_LOAD FLAGS(6);
    dynamic     PT_DYNAMIC FLAGS(6);
    tls         PT_TLS FLAGS(4);
    a20_native  0x6a20a200 FLAGS(0);
}

SECTIONS {
    . = SIZEOF_HEADERS;

    .interp : { *(.interp) } :interp_load :interp
    .dynsym : { *(.dynsym) } :interp_load
    .dynstr : { *(.dynstr) } :interp_load
    .gnu.hash : { *(.gnu.hash) } :interp_load
    .hash : { *(.hash) } :interp_load

    . = ALIGN(0x1000);
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

    .dynamic : { *(.dynamic) } :data :dynamic

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
