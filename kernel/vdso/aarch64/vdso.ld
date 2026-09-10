/* A20OS vDSO linker script (aarch64).
 *
 * Linked at vaddr 0 with the ELF header and program headers inside the first
 * (and only) PT_LOAD, so file offset == virtual offset and the raw binary can
 * be mapped verbatim.  Unlike the riscv64/loongarch64 scripts this does not
 * declare PHDRS explicitly: ld.bfd rejects an explicit PT_DYNAMIC here
 * ("first section in the PT_DYNAMIC segment is not the .dynamic section"), so
 * the default layout is used and the linker derives PT_DYNAMIC itself.
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

SECTIONS
{
    . = SIZEOF_HEADERS;
    .text : { *(.text*) }
    . = ALIGN(16);
    .dynamic : { *(.dynamic) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .rela.dyn : { *(.rela*) }

    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
        *(.data*)
        *(.bss*)
    }
}
