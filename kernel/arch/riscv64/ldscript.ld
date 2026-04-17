/*
 * A20OS linker script for RISC-V 64-bit (Higher-Half Kernel)
 *
 * Strategy: text/rodata/data are loaded at PA 0x80200000 (LMA).
 * Their VMA is 0xFFFFFFC080200000 (HH).  QEMU -kernel kernel.elf
 * loads segments to their LMA, which falls within the physical RAM
 * region that OpenSBI expects (PA 0x80200000+).
 *
 * BSS is not emitted as a loadable segment (NOLOAD), so it doesn't
 * inflate the ELF or confuse QEMU's memory layout.  entry.S zeroes
 * BSS manually before jumping to kernel_main.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

PHYS_BASE  = 0x80200000;
VIRT_BASE  = 0xFFFFFFC080200000;
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

SECTIONS
{
    . = VIRT_BASE;

    .text : AT(ADDR(.text) - LOAD_DELTA) {
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - LOAD_DELTA) {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - LOAD_DELTA) {
        *(.data .data.*)
        *(.sdata .sdata.*)
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
        *(.note .note.*)
        *(.comment)
        *(.gnu.hash)
    }
}
