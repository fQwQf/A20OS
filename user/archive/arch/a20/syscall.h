/*
 * musl arch/a20/syscall.h — RISC-V 64 ecall wrappers for A20 Native ABI.
 * Placed into musl's arch/a20/syscall.h for musl porting.
 *
 * A20 uses ecall with a7=syscall_nr, a0-a5=args, return in a0.
 * Same calling convention as Linux RISC-V.
 */
#ifndef _A20_ARCH_SYSCALL_H
#define _A20_ARCH_SYSCALL_H

#define __syscall0(n) ({ \
    register long a7 __asm__("a7") = (n); \
    register long a0 __asm__("a0"); \
    __asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory"); \
    a0; \
})

#define __syscall1(n,a) ({ \
    register long a7 __asm__("a7") = (n); \
    register long _a0 __asm__("a0") = (long)(a); \
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory"); \
    _a0; \
})

#define __syscall2(n,a,b) ({ \
    register long a7 __asm__("a7") = (n); \
    register long _a0 __asm__("a0") = (long)(a); \
    register long _a1 __asm__("a1") = (long)(b); \
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1) : "memory"); \
    _a0; \
})

#define __syscall3(n,a,b,c) ({ \
    register long a7 __asm__("a7") = (n); \
    register long _a0 __asm__("a0") = (long)(a); \
    register long _a1 __asm__("a1") = (long)(b); \
    register long _a2 __asm__("a2") = (long)(c); \
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2) : "memory"); \
    _a0; \
})

#define __syscall4(n,a,b,c,d) ({ \
    register long a7 __asm__("a7") = (n); \
    register long _a0 __asm__("a0") = (long)(a); \
    register long _a1 __asm__("a1") = (long)(b); \
    register long _a2 __asm__("a2") = (long)(c); \
    register long _a3 __asm__("a3") = (long)(d); \
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3) : "memory"); \
    _a0; \
})

#define __syscall5(n,a,b,c,d,e) ({ \
    register long a7 __asm__("a7") = (n); \
    register long _a0 __asm__("a0") = (long)(a); \
    register long _a1 __asm__("a1") = (long)(b); \
    register long _a2 __asm__("a2") = (long)(c); \
    register long _a3 __asm__("a3") = (long)(d); \
    register long _a4 __asm__("a4") = (long)(e); \
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4) : "memory"); \
    _a0; \
})

#define __syscall6(n,a,b,c,d,e,f) ({ \
    register long a7 __asm__("a7") = (n); \
    register long _a0 __asm__("a0") = (long)(a); \
    register long _a1 __asm__("a1") = (long)(b); \
    register long _a2 __asm__("a2") = (long)(c); \
    register long _a3 __asm__("a3") = (long)(d); \
    register long _a4 __asm__("a4") = (long)(e); \
    register long _a5 __asm__("a5") = (long)(f); \
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4), "r"(_a5) : "memory"); \
    _a0; \
})

#define VDSO_USE_CLOCK
#define IPC_64

#endif
