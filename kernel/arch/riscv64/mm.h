#ifndef _ARCH_RISCV64_MM_H
#define _ARCH_RISCV64_MM_H

#include "types.h"

/* SV39 page table constants */
#define ARCH_PT_LEVELS    3
#define ARCH_PT_ROOT_LEVEL 2
#define ARCH_PT_BITS      9
#define ARCH_PT_ENTRIES   512
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END  256

/* PTE flag bits (RISC-V Sv39 hardware format) */
#define PTE_V    (1UL << 0)
#define PTE_R    (1UL << 1)
#define PTE_W    (1UL << 2)
#define PTE_X    (1UL << 3)
#define PTE_U    (1UL << 4)
#define PTE_G    (1UL << 5)
#define PTE_A    (1UL << 6)
#define PTE_D    (1UL << 7)
#define PTE_COW  (1UL << 8)

/* LoongArch compat aliases — no hardware equivalent on RISC-V */
#define PTE_MAT1  0UL
#define PTE_LEAF  0UL

#define PTE_KERN (PTE_V | PTE_R | PTE_W | PTE_X)
#define PTE_USER (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U)

static inline int arch_pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) && ((pte & PTE_R) || (pte & PTE_W) || (pte & PTE_X));
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    return (int)((va >> (12 + 9 * level)) & 0x1FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return (pte >> 10) & 0x3FFFFFFFFFFFFFUL;
}

static inline uint64_t arch_pte_addr(uint64_t pte) {
    return arch_pte_ppn(pte) << 12;
}

static inline uint64_t arch_pte_from_pa(uint64_t pa) {
    return (pa >> 12) << 10;
}

static inline uint64_t *arch_pte_to_ptr(uint64_t pte) {
    return (uint64_t *)(arch_pte_addr(pte) + PAGE_OFFSET);
}

static inline uint64_t arch_make_satp(void *pgdir) {
    return (0x8UL << 60) | (((uint64_t)(uintptr_t)pgdir - PAGE_OFFSET) >> 12);
}

#endif
