OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = FLASH_LENGTH - CLUSTER_NVRAM_LENGTH
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = RAM_LENGTH
    SHARED (rw) : ORIGIN = 0x60000000, LENGTH = SHARED_LENGTH
}
SECTIONS {
    .isr_vector : { . = ALIGN(256); KEEP(*(.isr_vector)) } > FLASH
    .text : {
        *(.text.entry) *(.text .text.*) *(.rodata .rodata.*)
        . = ALIGN(4); _etext = .;
    } > FLASH
    /* Reset_Handler initializes only chip-private state. */
    .private_data : AT(_etext) {
        . = ALIGN(4); _data_start = .;
        *(.private.data .private.data.*) *(.ramfunc .ramfunc.*)
        . = ALIGN(4); _data_end = .;
    } > RAM
    _data_load = LOADADDR(.private_data);
    .private_bss (NOLOAD) : {
        . = ALIGN(8); _bss_start = .;
        *(.private.bss .private.bss.*)
        . = ALIGN(8); _bss_end = .;
    } > RAM
    /* Only the fabric's cold-boot owner initializes shared state. */
    .data : AT(LOADADDR(.private_data) + SIZEOF(.private_data)) {
        . = ALIGN(8); _shared_data_start = .;
        *(.data .data.*)
        . = ALIGN(8); _shared_data_end = .;
    } > SHARED
    _shared_data_load = LOADADDR(.data);
    _flash_image_end = LOADADDR(.data) + SIZEOF(.data);
    .bss (NOLOAD) : {
        . = ALIGN(64); _shared_bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8); _shared_bss_end = .;
    } > SHARED
    _heap_start = _shared_bss_end;
    _heap_end = ORIGIN(SHARED) + LENGTH(SHARED);
    _ram_origin = ORIGIN(RAM);
    _ram_linked_end = ORIGIN(RAM) + LENGTH(RAM);
    _stack_top = _ram_linked_end;
    _stack_limit = _stack_top - 8192;
    _cluster_nv_start = ORIGIN(FLASH) + LENGTH(FLASH);
    _cluster_nv_end = ORIGIN(FLASH) + FLASH_LENGTH;
    _flash_origin = ORIGIN(FLASH);
    _flash_linked_end = ORIGIN(FLASH) + LENGTH(FLASH);
    ASSERT(_bss_end <= _stack_limit, "private SRAM/boot stack overflow")
    ASSERT(_heap_end - _heap_start >= 32768, "shared heap too small")
    ASSERT(SHARED_LENGTH <= 0x04000000, "shared SRAM overlaps fabric registers")
    ASSERT(_flash_image_end <= _flash_linked_end, "firmware flash overflow")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
}
