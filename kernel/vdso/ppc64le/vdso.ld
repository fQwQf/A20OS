/* A20OS vDSO linker script (ppc64le).
 *
 * Linked at vaddr 0 with the ELF header and program headers inside the first
 * (and only) PT_LOAD, so file offset == virtual offset and the raw binary can
 * be mapped verbatim.  A PT_DYNAMIC segment is required by musl's vDSO parser.
 *
 * The exported symbols carry a LINUX_2.6.15 version definition (man 7 vdso:
 * ppc64), like the real kernel's vDSO.  Go's runtime (runtime/vdso_linux.go,
 * vdsoFindVersion) dereferences the verdef pointer unconditionally, so a
 * vDSO without DT_VERDEF segfaults every dynamically linked Go binary at
 * startup; with the definition present consumers match the versioned symbols
 * through DT_VERSYM and fall back to plain syscalls on any mismatch instead
 * of failing.
 */
OUTPUT_FORMAT("elf64-powerpcle")
OUTPUT_ARCH(powerpc:common64)

VERSION
{
    LINUX_2.6.15
    {
        global:
            __vdso_clock_gettime;
            __vdso_gettimeofday;
            __vdso_getcpu;
        local: *;
    };
}

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
        *(.eh_frame*)
        *(.data*)
        *(.bss*)
    }
}
