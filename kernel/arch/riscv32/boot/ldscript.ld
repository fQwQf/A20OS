OUTPUT_ARCH(riscv)
ENTRY(_start)

PROVIDE(PHYS_BASE = 0x80200000);
PROVIDE(VIRT_BASE = 0x80200000);
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
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
        /* The BSP boot stack goes first, so it sits at the bottom of .bss and
         * no other kernel object can be placed inside its address range.  It
         * has to live *inside* .bss: everything from _bss_end upwards belongs
         * to the physical frame allocator (mm_init passes va_to_pa(_bss_end)
         * to pfa_init, which zeroes its frame-metadata bitmap starting at
         * ROUND_UP(kernel_end, 64)), so a stack reserved above _bss_end is
         * memset out from under itself the moment the allocator comes up.
         *
         * KEEP(*(.bss.stack)) must stay *before* *(.bss .bss.*): that pattern
         * also matches .bss.stack, and GNU ld applies the first match, so
         * placing it later would silently make this directive dead code. */
        KEEP(*(.bss.stack))
        . = ALIGN(4096);
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    } :data

    /* Both invariants of the layout above, stated so a later edit that breaks
     * either fails the link instead of booting into silent corruption. */
    ASSERT(_stack_bottom == _bss_start,
           "riscv32: boot stack must be the first thing in .bss")
    ASSERT(_stack_end <= _bss_end,
           "riscv32: boot stack must stay below _bss_end (pfa owns everything above it)")
    ASSERT(_stack_end - _stack_bottom >= 64 * 1024,
           "riscv32: boot stack is below the 64 KiB every arch reserves")

    . = ALIGN(16);
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
