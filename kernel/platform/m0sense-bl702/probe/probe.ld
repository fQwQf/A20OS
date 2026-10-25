/* Conservative subset of BouffaloSDK bl702_ram.ld.  Executable SRAM and
 * data SRAM aliases are distinct physical regions here; no XIP/ROM headers. */
OUTPUT_ARCH(riscv)
ENTRY(probe_start)
MEMORY {
    code (rx) : ORIGIN = 0x22010000, LENGTH = 64K
    data (rw) : ORIGIN = 0x42020000, LENGTH = 16K
}
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text .text.*) *(.rodata .rodata.*) } > code
    .data : { . = ALIGN(4); __global_pointer$ = . + 0x800;
              *(.data .data.* .sdata .sdata.*) } > data
    .bss (NOLOAD) : {
        . = ALIGN(4); __bss_start = .;
        *(.bss .bss.* .sbss .sbss.*) *(COMMON)
        . = ALIGN(4); __bss_end = .;
    } > data
    __stack_top = ORIGIN(data) + LENGTH(data);
    __stack_bottom = __stack_top - 4096;
    ASSERT(__bss_end <= __stack_bottom, "BL702 probe data overlaps 4 KiB stack")
    ASSERT(SIZEOF(.text) <= LENGTH(code), "BL702 probe exceeds executable SRAM")
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
