/*
 * A20OS linker script for RISC-V 64-bit
 * QEMU virt machine with OpenSBI loads kernel at 0x80200000
 * We run in physical address space (no high-half kernel mapping)
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

BASE_ADDRESS = 0x80200000;

SECTIONS
{
    . = BASE_ADDRESS;

    /* Code section - entry point first */
    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    /* Read-only data */
    .rodata : {
        *(.rodata .rodata.*)
    }

    /* Initialized data */
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    /* BSS - zero-initialized data */
    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* Boot stack (used during early init only) */
    . = ALIGN(16);

    /* Discard unneeded sections */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
        *(.gnu.hash)
    }
}
