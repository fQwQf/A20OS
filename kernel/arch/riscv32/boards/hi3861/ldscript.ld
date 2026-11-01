OUTPUT_ARCH(riscv)
ENTRY(_start)

/*
 * Kernel A is executed in place from SPI flash.  The payload starts after the
 * 0x3c0-byte Hi3861 firmware header, and only .data/.bss/heap/stack live in
 * the SRAM window below the ROM loader's fixed data at 0x11a9c0.
 */
MEMORY {
    FLASH (rx) : ORIGIN = 0x0040D3C0, LENGTH = 0xE3C40
    RAM   (rw) : ORIGIN = 0x000D8000, LENGTH = 0x42000
}

SECTIONS {
    /* FlashBoot jumps to exactly 0x0040d3c0: keep the entry section first
     * and let the 256-byte-aligned vector table start after it. */
    .text.entry : {
        KEEP(*(.text.entry))
        . = ALIGN(4);
    } > FLASH

    .text.vectors : ALIGN(256) {
        KEEP(*(.text.vectors))
        . = ALIGN(4);
    } > FLASH

    .text : {
        *(.text .text.*)
        . = ALIGN(4);
    } > FLASH

    .rodata : {
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(16);
        _etext = .;
    } > FLASH

    .data : AT(_etext) {
        . = ALIGN(16);
        _data_start = .;
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(16);
        _data_end = .;
    } > RAM
    _data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(16);
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        _bss_end = .;
    } > RAM

    _heap_start = ALIGN(16);
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    _heap_end = _stack_top - 8192;
    ASSERT(_heap_end - _heap_start >= 65536, "Hi3861 heap below 64 KiB")
    ASSERT(_etext + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH),
           "Hi3861 kernel A exceeds the 912 KiB partition")
    /DISCARD/ : { *(.eh_frame .comment .note.*) }
}
