#ifndef A20_BSD_MACHINE_ELF_H
#define A20_BSD_MACHINE_ELF_H
#include_next <machine/elf.h>
#if defined(A20_BSD_PORTABLE_LP64)
#if __SIZEOF_POINTER__ != 8 || __SIZEOF_LONG__ != 8
#error The network LP64 personality requires 64-bit pointers and longs
#endif
#undef ELF_ARCH
#undef ELF_TARG_MACH
#if defined(__riscv) && __riscv_xlen == 64
#define ELF_ARCH EM_RISCV
#elif defined(__loongarch64)
/* This machine ID postdates the vendored FreeBSD 13 ELF definitions. */
#ifndef EM_LOONGARCH
#define EM_LOONGARCH 258
#endif
#define ELF_ARCH EM_LOONGARCH
#else
#error Unsupported network personality target
#endif
#define ELF_TARG_MACH ELF_ARCH
#endif
#endif
