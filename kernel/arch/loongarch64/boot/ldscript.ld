/* LoongArch64 Linker Script
 * Kernel loaded at 0x200000 physical (QEMU loongarch64 virt default)
 * TODO: 将0x2000000改回0x200000
 */

OUTPUT_ARCH(loongarch)
ENTRY(_start)

SECTIONS {
    . = 0x2000000;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    . = ALIGN(4096);
    .bss : {
        /* Export both GNU-style and project-style BSS symbols. */
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(.bss.stack)
        . = ALIGN(4096);
        _bss_end = .;
    }

    _end = .;

    /* Discard unnecessary sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
