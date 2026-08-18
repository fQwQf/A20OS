/* Identity-linked VisionFive 2 image for first hardware bring-up.
 * OpenSBI enters at PA 0x40200000; with CONFIG_NOMMU the kernel's virtual
 * addresses are physical addresses, so no satp transition is required. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

PROVIDE(PHYS_BASE = 0x40200000);
PROVIDE(VIRT_BASE = 0x40200000);
PROVIDE(BOOT_MAP_PHYS = 0x40000000);
PROVIDE(BOOT_MAP_MMIO_HI = 0x0);
LOAD_DELTA = 0;

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = PHYS_BASE;

    .text : {
        *(.text.entry)
        *(.text .text.*)
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

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
        *(.gnu.hash)
    }
}
