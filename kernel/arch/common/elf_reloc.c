#include "mm/elf.h"
#include <stdint.h>

/*
 * The relocation types this loader applies, per machine.
 *
 * A static-pie image needs two families and no more.  RELATIVE covers every
 * pointer whose target the linker already knows: the word becomes the load
 * base plus the link-time address.  The ABS64 family covers the pointers the
 * linker had to leave symbolic -- in mksh that is the keyword and operator
 * string tables its parser dispatches through, so skipping them yields a shell
 * that runs simple commands but cannot recognise `while', `case', or `{' --
 * and resolves the symbol out of .dynsym to get the address.  There is no PLT
 * to fill and no symbol to bind to an outside object, so nothing else is
 * reachable from .rela.dyn.
 */
#define R_ARM_RELATIVE 23
#define R_X86_64_RELATIVE 8
#define R_AARCH64_RELATIVE 1027
#define R_RISCV_RELATIVE 3
#define R_PPC64_RELATIVE 1
#define R_LOONGARCH_RELATIVE 2

#define R_ARM_ABS32 2
#define R_X86_64_64 1
#define R_AARCH64_ABS64 257
#define R_RISCV_64 2
#define R_PPC64_ADDR64 38
#define R_LOONGARCH_64 3

uint32_t arch_elf_relative_reltype(uint16_t machine) {
    switch (machine) {
        case EM_ARM:
            return R_ARM_RELATIVE;
        case EM_X86_64:
            return R_X86_64_RELATIVE;
        case EM_AARCH64:
            return R_AARCH64_RELATIVE;
        case EM_RISCV:
            return R_RISCV_RELATIVE;
        case EM_PPC64:
            return R_PPC64_RELATIVE;
        case EM_LOONGARCH:
            return R_LOONGARCH_RELATIVE;
        default:
            return __UINT32_MAX__;
    }
}

/* The ABS64 (or ABS32 on a 32-bit machine) relocation type, or __UINT32_MAX__. */
uint32_t arch_elf_absolute_reltype(uint16_t machine, bool elf64) {
    switch (machine) {
        case EM_ARM:
            return elf64 ? __UINT32_MAX__ : R_ARM_ABS32;
        case EM_X86_64:
            return elf64 ? R_X86_64_64 : __UINT32_MAX__;
        case EM_AARCH64:
            return elf64 ? R_AARCH64_ABS64 : __UINT32_MAX__;
        case EM_RISCV:
            return elf64 ? R_RISCV_64 : __UINT32_MAX__;
        case EM_PPC64:
            return elf64 ? R_PPC64_ADDR64 : __UINT32_MAX__;
        case EM_LOONGARCH:
            return elf64 ? R_LOONGARCH_64 : __UINT32_MAX__;
        default:
            return __UINT32_MAX__;
    }
}
