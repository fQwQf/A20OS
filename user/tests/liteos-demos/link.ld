OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS {
    code PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
    . = SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) } :code
    .dynsym : { *(.dynsym) } :code
    .dynstr : { *(.dynstr) } :code
    .hash : { *(.hash) } :code
    .gnu.hash : { *(.gnu.hash) } :code
    .rela.dyn : { *(.rela.dyn) *(.rela.*) } :code
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got .got.*) } :data
    .data : { *(.data .data.*) *(.sdata .sdata.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) *(.sbss .sbss.*) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.riscv.attributes) *(.eh_frame*) }
}
