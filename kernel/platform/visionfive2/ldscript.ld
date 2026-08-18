/*
 * A20OS linker script for RISC-V 64-bit on StarFive VisionFive 2 (JH7110).
 *
 * Same layout strategy as the arch default (kernel/arch/riscv64/boot/
 * ldscript.ld), but the JH7110 DRAM window starts at PA 0x40000000 instead
 * of QEMU virt's 0x80000000.  OpenSBI/U-Boot SPL loads the kernel image at
 * PA 0x40200000 (LMA); the VMA is the corresponding higher-half address.
 *
 * BOOT_MAP_PHYS/BOOT_MAP_MMIO_HI feed the entry.S boot page table: sixteen
 * 1 GiB RAM megapages start at index 0x40000000 >> 30 = 1, and the second
 * higher-half MMIO slot is disabled because PA 0x40000000 is RAM on this SoC
 * (all JH7110 devices live below DRAM and are covered by the PA 0..1 GiB
 * higher-half MMIO megapage).
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

PROVIDE(PHYS_BASE = 0x40200000);
PROVIDE(VIRT_BASE = 0xFFFFFFC040200000);
PROVIDE(BOOT_MAP_PHYS = 0x40000000);
PROVIDE(BOOT_MAP_MMIO_HI = 0x0);
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

PHDRS
{
    text   PT_LOAD FLAGS(5 /* R-X */);
    rodata PT_LOAD FLAGS(4 /* R-- */);
    data   PT_LOAD FLAGS(6 /* RW- */);
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
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    } :data

    . = ALIGN(16);
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
        *(.gnu.hash)
    }
}
