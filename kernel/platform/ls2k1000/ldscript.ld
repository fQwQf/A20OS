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
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        *(.bss.stack)
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

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
