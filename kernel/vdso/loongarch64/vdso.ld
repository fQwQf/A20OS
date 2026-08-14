/* A20OS vDSO linker script (loongarch64).
 *
 * Mirrors the riscv64 image layout: linked at vaddr 0 with the ELF header and
 * program headers inside the first (and only) PT_LOAD, so file offset ==
 * virtual offset and the raw binary can be mapped verbatim.  A PT_DYNAMIC
 * segment is required by musl's vDSO parser; symbols are unversioned, which
 * musl accepts (a missing verdef disables its version check).
 */
OUTPUT_FORMAT("elf64-loongarch")
OUTPUT_ARCH(loongarch)

PHDRS
{
    load PT_LOAD FLAGS(5) FILEHDR PHDRS; /* R + X */
    dyn  PT_DYNAMIC FLAGS(4);            /* R */
}

SECTIONS
{
    . = 0 + SIZEOF_HEADERS;
    .text : { *(.text*) } :load
    . = ALIGN(16);
    .dynamic : { *(.dynamic) } :load :dyn
    .hash : { *(.hash) } :load
    .gnu.hash : { *(.gnu.hash) } :load
    .dynsym : { *(.dynsym) } :load
    .dynstr : { *(.dynstr) } :load
    .rela.dyn : { *(.rela*) } :load

    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.loongarch.attributes)
        *(.eh_frame*)
        *(.data*)
        *(.bss*)
    }
}
