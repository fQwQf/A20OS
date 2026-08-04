    .text
    .global _start
    .type _start, @function
_start:
    /* ELFv2 needs r2 set to the TOC base before any C code runs.  r12 is
     * guaranteed to hold the entry address by the native ABI loader. */
    addis 2, 12, .TOC.-_start@ha
    addi  2, 2, .TOC.-_start@l
    bl _start_c
    li 0, 0x0200
    li 3, 0
    sc
1:  b 1b
    .size _start, . - _start
