OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = FLASH_LENGTH
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = RAM_LENGTH
}

SECTIONS
{
    .isr_vector : {
        . = ALIGN(256);
        KEEP(*(.isr_vector))
    } > FLASH

    .text : {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .data : AT(_etext) {
        . = ALIGN(4);
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } > RAM
    _data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM

    _heap_start = _bss_end;
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    _stack_limit = _stack_top - 2048;
    _heap_end = _stack_limit;

    ASSERT(_heap_start <= _heap_end, "STM32 SRAM overflow")
    ASSERT(SIZEOF(.isr_vector) + SIZEOF(.text) + SIZEOF(.data) <= LENGTH(FLASH),
           "STM32 flash overflow")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
