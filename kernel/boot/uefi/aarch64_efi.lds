OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(efi_main)

SECTIONS
{
    . = 0;
    ImageBase = .;
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    . = ALIGN(4096);
    .text : { *(.text .text.*) }
    . = ALIGN(65536);
    .reloc : {
        /* PE/COFF requires a base-relocation directory even though all code
         * emitted by this loader is position independent. */
        LONG(0x2000); LONG(12); SHORT(0); SHORT(0);
    }
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) }
    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        *(.got .got.*)
        *(.bss .bss.* COMMON)
    }
    . = ALIGN(4096);
    .kernel : { *(.kernel) }
    . = ALIGN(4096);
    .rela : { *(.rela .rela.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .dynsym : { *(.dynsym) }
    . = ALIGN(4096);
    .dynstr : { *(.dynstr) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
