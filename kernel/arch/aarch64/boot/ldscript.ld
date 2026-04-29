OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHYS_BASE  = 0x40080000;
VIRT_BASE  = 0x0000008040080000;
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R-X */
    rodata PT_LOAD FLAGS(4); /* R-- */
    data   PT_LOAD FLAGS(6); /* RW- */
}

SECTIONS
{
    . = VIRT_BASE;

    .text : AT(ADDR(.text) - LOAD_DELTA) {
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - LOAD_DELTA) {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - LOAD_DELTA) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    .bss (NOLOAD) : AT(ADDR(.bss) - LOAD_DELTA) {
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
        *(.interp)
        *(.gnu.hash)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.got*)
    }
}
