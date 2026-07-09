OUTPUT_ARCH(arm)
ENTRY(_start)

PROVIDE(PHYS_BASE = 0x40080000);
PROVIDE(VIRT_BASE = 0xC0080000);
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

PHDRS
{
    text_entry PT_LOAD FLAGS(5);
    text       PT_LOAD FLAGS(5);
    rodata     PT_LOAD FLAGS(4);
    data       PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = PHYS_BASE;

    .text.entry : AT(PHYS_BASE) {
        *(.text.entry)
    } :text_entry

    _text_entry_end = .;

    . = VIRT_BASE + (_text_entry_end - PHYS_BASE);

    .text : AT(ADDR(.text) - LOAD_DELTA) {
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

    . = ALIGN(8);
    .driver_init : AT(ADDR(.driver_init) - LOAD_DELTA) {
        __driver_init_start = .;
        KEEP(*(.driver_init))
        __driver_init_end = .;
    } :data

    .board_init : AT(ADDR(.board_init) - LOAD_DELTA) {
        __board_init_start = .;
        KEEP(*(.board_init))
        __board_init_end = .;
    } :data

    .bss (NOLOAD) : AT(ADDR(.bss) - LOAD_DELTA) {
        _bss_start = .;
        *(.bss.stack)
        . = ALIGN(4096);
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
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
