OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5 /* R-X */);
    rodata PT_LOAD FLAGS(4 /* R-- */);
    data   PT_LOAD FLAGS(6 /* RW- */);
}

SECTIONS {
    . = 0x80000000;

    .text : {
        *(.text.entry)
        *(.text .text.*)
        /* EENTRY / TLBRENTRY zero the low 6 address bits, so both exception
         * entry points must land on a 64-byte boundary.  Keep the trap code
         * inside the .text PT_LOAD so the flat image (objcopy -O binary)
         * carries it, and align it here because the la32 ld does not honor
         * input-section alignment. */
        . = ALIGN(64);
        *(.trapcode)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    . = ALIGN(8);
    .driver_init : {
        __driver_init_start = .;
        KEEP(*(.driver_init))
        __driver_init_end = .;
    } :data

    .board_init : {
        __board_init_start = .;
        KEEP(*(.board_init))
        __board_init_end = .;
    } :data

    . = ALIGN(4096);
    .bss (NOLOAD) : {
        _bss_start = .;
        /* The BSP boot stack must be the FIRST input collected here.
         * *(.bss .bss.*) already matches the .bss.stack input section and GNU
         * ld applies the first matching directive, so a *(.bss.stack) placed
         * after that wildcard is dead code -- the stack used to land
         * wherever the general .bss run put it, sharing its pages with
         * unrelated kernel objects.  Collect it first and the 64 KiB range
         * sits at the bottom of .bss, where nothing else can be placed.
         *
         * It also has to stay inside .bss, below _bss_end: mm_init hands
         * va_to_pa(_bss_end) to pfa_init, which zeroes its multi-megabyte
         * frame-metadata bitmap starting at ROUND_UP(kernel_end, 64)
         * (mm/frame.c), so anything reserved at or above _bss_end is memset
         * out from under itself once the allocator comes up. */
        KEEP(*(.bss.stack))
        . = ALIGN(4096);
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    } :data

    _end = .;

    /* Both invariants of the layout above, stated so a later edit that breaks
     * either fails the link instead of booting into silent corruption. */
    ASSERT(boot_stack == _bss_start,
           "loongarch32: boot stack must be the first thing in .bss")
    ASSERT(boot_stack_top <= _bss_end,
           "loongarch32: boot stack must stay below _bss_end (pfa owns everything above it)")
    ASSERT(boot_stack_top - boot_stack >= 64 * 1024,
           "loongarch32: boot stack is below the 64 KiB every arch reserves")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
