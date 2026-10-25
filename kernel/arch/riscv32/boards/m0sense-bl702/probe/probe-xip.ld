/* Application stream for the FACTORY M0SENSE UF2 loader, not ROM boot images.
 * Sipeed's official examples start at 0x23000000 and contain no bootheader.
 * The UF2 loader owns flash partition placement; targetAddr is app-relative. */
OUTPUT_ARCH(riscv)
ENTRY(xip_start)
MEMORY {
    flash (rx) : ORIGIN = 0x23000000, LENGTH = 64K
    code (rx) : ORIGIN = 0x22010000, LENGTH = 64K
    data (rw) : ORIGIN = 0x42020000, LENGTH = 16K
}
PHDRS {
    boot PT_LOAD FLAGS(5);
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    .xip_entry : { KEEP(*(.text.xip_entry)) . = ALIGN(4); } > flash :boot
    .text : {
        __text_run = .;
        KEEP(*(.text.entry)) *(.text .text.*) *(.rodata .rodata.*)
        . = ALIGN(4); __text_end = .;
    } > code AT> flash :code
    __text_load = LOADADDR(.text);
    .data : {
        __data_run = .; __global_pointer$ = . + 0x800;
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4); __data_end = .;
    } > data AT> flash :data
    __data_load = LOADADDR(.data);
    .bss (NOLOAD) : {
        . = ALIGN(4); __bss_start = .;
        *(.bss .bss.* .sbss .sbss.*) *(COMMON)
        . = ALIGN(4); __bss_end = .;
    } > data :data
    __stack_top = ORIGIN(data) + LENGTH(data);
    __stack_bottom = __stack_top - 4096;
    ASSERT(xip_start == 0x23000000, "M0Sense factory loader entry must be 0x23000000")
    ASSERT(__bss_end <= __stack_bottom, "BL702 data overlaps 4 KiB stack")
    ASSERT(__data_load + SIZEOF(.data) <= ORIGIN(flash) + LENGTH(flash), "UF2 application exceeds 64 KiB budget")
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
