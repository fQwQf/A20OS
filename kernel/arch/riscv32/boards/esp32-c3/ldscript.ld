OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
    IRAM (rx) : ORIGIN = 0x40380000, LENGTH = 128K
    DRAM (rw) : ORIGIN = 0x3fca0000, LENGTH = 240K
}
SECTIONS {
    .text : { KEEP(*(.text.entry)) KEEP(*(.text.vectors)) *(.text .text.*) } > IRAM
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > DRAM
    .data : { *(.data .data.* .sdata .sdata.*) . = ALIGN(16); } > DRAM
    .bss (NOLOAD) : { _bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(16); _bss_end = .; } > DRAM
    _heap_start = ALIGN(16);
    _stack_top = ORIGIN(DRAM) + LENGTH(DRAM);
    _heap_end = _stack_top - 8192;
    ASSERT(_heap_end - _heap_start >= 65536, "ESP32-C3 heap below 64 KiB")
    ASSERT(SIZEOF(.text) <= LENGTH(IRAM), "ESP32-C3 IRAM budget exhausted")
    /DISCARD/ : { *(.eh_frame .comment .note.*) }
}
