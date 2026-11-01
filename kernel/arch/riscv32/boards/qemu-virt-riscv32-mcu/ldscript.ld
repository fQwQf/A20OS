OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 320K }
SECTIONS {
    .text : { KEEP(*(.text.entry)) KEEP(*(.text.vectors)) *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > RAM
    .data : { *(.data .data.* .sdata .sdata.*) } > RAM
    .bss (NOLOAD) : { . = ALIGN(16); _bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(16); _bss_end = .; } > RAM
    _heap_start = ALIGN(16);
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    _heap_end = _stack_top - 8192;
    ASSERT(_heap_end - _heap_start >= 32768, "MCU heap budget exhausted")
    /DISCARD/ : { *(.eh_frame .comment .note.*) }
}
