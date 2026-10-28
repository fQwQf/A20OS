/* A20OS vDSO linker script (aarch64).
 *
 * Linked at vaddr 0 with the ELF header and program headers inside the first
 * (and only) PT_LOAD, so file offset == virtual offset and the raw binary can
 * be mapped verbatim.  Unlike the riscv64/loongarch64 scripts this does not
 * declare PHDRS explicitly: ld.bfd rejects an explicit PT_DYNAMIC here
 * ("first section in the PT_DYNAMIC segment is not the .dynamic section"), so
 * the default layout is used and the linker derives PT_DYNAMIC itself.
 *
 * The exported symbols carry a LINUX_2.6.39 version definition (man 7 vdso:
 * aarch64), like the real kernel's vDSO.  Go's runtime (runtime/vdso_linux.go,
 * vdsoFindVersion) dereferences the verdef pointer unconditionally, so a
 * vDSO without DT_VERDEF segfaults every dynamically linked Go binary at
 * startup; with the definition present consumers match the versioned symbols
 * through DT_VERSYM and fall back to plain syscalls on any mismatch instead
 * of failing.
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

VERSION
{
    LINUX_2.6.39
    {
        global:
            __vdso_clock_gettime;
            __vdso_gettimeofday;
            __vdso_getcpu;
        local: *;
    };
}

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
