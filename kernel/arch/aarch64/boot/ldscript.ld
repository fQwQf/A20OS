OUTPUT_ARCH(aarch64)
ENTRY(_start_phys)

PROVIDE(PHYS_BASE = 0x40080000);
PROVIDE(VIRT_BASE = 0x0000008040080000);
LOAD_DELTA = VIRT_BASE - PHYS_BASE;

_start_phys = _start - LOAD_DELTA;

PHDRS
{
    text   PT_LOAD FLAGS(5 /* R-X */);
    rodata PT_LOAD FLAGS(4 /* R-- */);
    data   PT_LOAD FLAGS(6 /* RW- */);
}

SECTIONS
{
    . = VIRT_BASE;
    __text_start = .;

    .text : AT(ADDR(.text) - LOAD_DELTA) {
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    __rodata_start = .;
    .rodata : AT(ADDR(.rodata) - LOAD_DELTA) {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    __data_start = .;
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
        *(.bss.stack)
        . = ALIGN(4096);
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    } :data

    _end = .;

    /*
     * NOMMU only: a 2 MiB-aligned arena for drvmod images.  Under NOMMU the
     * whole of DRAM is one flat identity map, and AArch64 makes a stage-1
     * block that grants EL0 write access execute-never for every higher EL,
     * so a single AP cannot serve both EL0 user memory and kernel code.  The
     * boot map therefore marks everything up to __drvmod_arena_end AP=00 and
     * the remainder AP=01, which forces drvmod images -- the only privileged
     * code the frame pool ever holds -- into this reserved prefix.
     *
     * These are absolute symbols: they occupy no output bytes.  The MMU
     * builds ignore them (pfa_init() starts at _bss_end and the boot map is
     * the normal high-half alias), so they cost nothing there.
     *
     * The size is DRV_MOD_MAX_MODULES (32) * DRV_MOD_MAX_SIZE (512 KiB)
     * exactly, so the arena is a hard guarantee rather than a guess; the
     * modules this platform actually loads need well under a megabyte.
     */
    __drvmod_arena_start = ALIGN(_end, 0x200000);
    __drvmod_arena_end = __drvmod_arena_start + 0x1000000;
    ASSERT(__drvmod_arena_start >= _end,
           "drvmod arena must start at or after the kernel image")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.interp)
        *(.gnu.hash)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.got*)
    }
}
