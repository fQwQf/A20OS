#ifndef A20_A64_EFI_LOADER_ARCH_H
#define A20_A64_EFI_LOADER_ARCH_H
static void sync_loaded_code(void *addr, size_t size) {
    uint64_t ctr;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    size_t dline = (size_t)4U << ((ctr >> 16) & 0xfU);
    size_t iline = (size_t)4U << (ctr & 0xfU);
    uintptr_t start = (uintptr_t)addr & ~((uintptr_t)dline - 1);
    uintptr_t end =
        ((uintptr_t)addr + size + dline - 1) & ~((uintptr_t)dline - 1);

    for (uintptr_t p = start; p < end; p += dline)
        __asm__ __volatile__("dc cvac, %0" ::"r"(p) : "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");

    start = (uintptr_t)addr & ~((uintptr_t)iline - 1);
    end = ((uintptr_t)addr + size + iline - 1) & ~((uintptr_t)iline - 1);
    for (uintptr_t p = start; p < end; p += iline)
        __asm__ __volatile__("ic ivau, %0" ::"r"(p) : "memory");
    __asm__ __volatile__("dsb ish\n\tisb" ::: "memory");
}

static __attribute__((noreturn)) void enter_kernel(uintptr_t entry,
                                                   uintptr_t rsdp,
                                                   uintptr_t cmdline,
                                                   uintptr_t handoff) {
    __asm__ __volatile__("dsb sy\n\t"
                         "mrs x1, sctlr_el1\n\t"
                         "bic x1, x1, #1\n\t"
                         "bic x1, x1, #(1 << 2)\n\t"
                         "bic x1, x1, #(1 << 12)\n\t"
                         "msr sctlr_el1, x1\n\t"
                         "isb\n\t"
                         "mov x0, %1\n\t"
                         "mov x2, %2\n\t"
                         "mov x3, %3\n\t"
                         "br %0" ::"r"(entry),
                         "r"(rsdp), "r"(cmdline), "r"(handoff)
                         : "x0", "x1", "x2", "x3", "memory");
    __builtin_unreachable();
}
#endif
