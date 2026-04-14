/* LoongArch64 Linker Script
 * Kernel loaded at 0x200000 physical (QEMU loongarch64 virt default)
 */

OUTPUT_ARCH(loongarch)
ENTRY(_start)

SECTIONS {
    . = 0x200000;

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
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(.bss.stack)
        . = ALIGN(4096);
        __bss_end = .;
    }

    _end = .;

    /* Discard unnecessary sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
