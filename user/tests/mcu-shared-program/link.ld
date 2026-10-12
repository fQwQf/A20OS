OUTPUT_ARCH(arm)
ENTRY(_start)
PHDRS {
    code PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
    . = SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    .dynsym : { *(.dynsym) } :code
    .dynstr : { *(.dynstr) } :code
    .hash : { *(.hash) } :code
    .gnu.hash : { *(.gnu.hash) } :code
    .rel.dyn : { *(.rel.dyn) } :code
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got .got.*) } :data
    .data : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
}
