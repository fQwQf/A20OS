OUTPUT_ARCH(powerpc:common64)
ENTRY(_start)

PROVIDE(PHYS_BASE = 0x00400000);
PROVIDE(VIRT_BASE = 0xC000800000800000);
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = VIRT_BASE;

    .text : AT(ADDR(.text) - LOAD_DELTA) {
        __text_start = .;
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - LOAD_DELTA) {
        __rodata_start = .;
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - LOAD_DELTA) {
        __data_start = .;
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
        /* The BSP boot stack must be the FIRST input collected here.
         * *(.bss .bss.*) already matches the .bss.stack input section and GNU
         * ld applies the first matching directive, so the old *(.bss.stack)
         * below the wildcard was dead code.  The stack therefore landed at the
         * end of .bss instead of the bottom: measured on this arch it put
         * _stack_end exactly at _bss_end (0xc0008000019d4000), zero margin to
         * the boundary where pfa_init starts zeroing its frame-metadata bitmap
         * (mm_init passes va_to_pa(_bss_end), mm/frame.c rounds that up to 64),
         * and it shared pages with whatever general .bss object happened to be
         * allocated against it.  Collect it first and the 64 KiB range sits at
         * the bottom of .bss, where nothing else can be placed and far below
         * the allocator's memset region. */
        KEEP(*(.bss.stack))
        . = ALIGN(4096);
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        *(.bss.page_tables)
        . = ALIGN(4096);
        _bss_end = .;
    } :data

    _end = .;

    /* Both invariants of the layout above, stated so a later edit that breaks
     * either fails the link instead of booting into silent corruption. */
    ASSERT(_stack_bottom == _bss_start,
           "ppc64le: boot stack must be the first thing in .bss")
    ASSERT(_stack_end <= _bss_end,
           "ppc64le: boot stack must stay below _bss_end (pfa owns everything above it)")
    ASSERT(_stack_end - _stack_bottom >= 64 * 1024,
           "ppc64le: boot stack is below the 64 KiB every arch reserves")

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
