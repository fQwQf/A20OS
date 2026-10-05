OUTPUT_ARCH(loongarch)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5 /* R-X */);
    rodata PT_LOAD FLAGS(4 /* R-- */);
    data   PT_LOAD FLAGS(6 /* RW- */);
}

SECTIONS {
    /* Cached DMW alias of physical 0x02000000, loaded by U-Boot at this VA. */
    . = 0x9000000002000000;

    .text : {
        *(.text.entry)
        . = ALIGN(4096);
        KEEP(*(.trap_entry))
        *(.text .text.*)
    } :text

    .tlb_refill ALIGN(4096) : {
        KEEP(*(.tlb_refill))
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
         * ld applies the first matching directive, so the *(.bss.stack) that
         * used to sit after that wildcard was dead code: the stack landed
         * wherever the general .bss run put it, sharing its pages with
         * unrelated kernel objects.  Collecting it first puts the 64 KiB per
         * CPU range at the bottom of .bss, where nothing else can be placed,
         * and keeps it below _bss_end -- mm_init hands va_to_pa(_bss_end) to
         * pfa_init, which zeroes its frame-metadata bitmap starting at
         * ROUND_UP(kernel_end, 64), so anything at or above _bss_end is memset
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

    ASSERT(_start == 0x9000000002000000,
           "LS2K1000 kernel entry moved from the verified load address")
    ASSERT((trap_entry_la64 & 0xfff) == 0,
           "LS2K1000 general exception entry must be 4 KiB aligned")
    ASSERT((tlb_refill_entry_la64 & 0xfff) == 0,
           "LoongArch TLB refill entry must be 4 KiB aligned")
    ASSERT(_end <= 0x900000000b000000,
           "LS2K1000 kernel overlaps the firmware/framebuffer safety boundary")
    ASSERT(boot_stack == _bss_start,
           "LS2K1000 boot stack must be the first thing in .bss")
    ASSERT(boot_stack_top <= _bss_end,
           "LS2K1000 boot stack must stay below _bss_end (pfa owns everything above it)")
    ASSERT(boot_stack_top - boot_stack >= 64 * 1024,
           "LS2K1000 boot stack is below the 64 KiB every arch reserves")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
