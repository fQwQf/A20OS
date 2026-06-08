OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHYS_BASE  = 0x00200000;
VIRT_BASE  = 0xFFFF800000200000;
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

SECTIONS
{
    . = VIRT_BASE;

    .text : AT(ADDR(.text) - LOAD_DELTA) {
        *(.multiboot)
        *(.text.entry)
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - LOAD_DELTA) {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - LOAD_DELTA) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    . = ALIGN(8);
    .driver_init : AT(ADDR(.driver_init) - LOAD_DELTA) {
        __driver_init_start = .;
        KEEP(*(.driver_init))
        __driver_init_end = .;
    }

    .board_init : AT(ADDR(.board_init) - LOAD_DELTA) {
        __board_init_start = .;
        KEEP(*(.board_init))
        __board_init_end = .;
    }

    .bss (NOLOAD) : AT(ADDR(.bss) - LOAD_DELTA) {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    }

    . = ALIGN(16);

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.comment)
        *(.gnu.hash)
    }
}
