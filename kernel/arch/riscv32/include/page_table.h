#ifndef _ARCH_RISCV32_MM_H
#define _ARCH_RISCV32_MM_H

#include "core/types.h"

typedef uint32_t rv32_pte_t;
typedef uint32_t pde_t;
typedef uint32_t pgd_t;

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (~(PAGE_SIZE - 1UL))
#endif

#define ARCH_PT_LEVELS 2
#define ARCH_PT_ROOT_LEVEL 1
#define ARCH_PT_BITS 10
#define ARCH_PT_ENTRIES 1024
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END 512

#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)
#define PTE_COW (1UL << 8)

#define PTE_MAT1 0UL
#define PTE_LEAF 0UL

#define PTE_KERN (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D)
#define PTE_USER (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)
#define PTE_DIR (PTE_V)

static inline int arch_pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X));
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    return (int)((va >> (12 + 10 * level)) & 0x3FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return ((uint32_t)pte >> 10) & 0x003FFFFFUL;
}

static inline uint64_t arch_pte_addr(uint64_t pte) {
    return arch_pte_ppn(pte) << 12;
}

static inline uint64_t arch_pte_from_pa(uint64_t pa) {
    return ((uint32_t)pa >> 12) << 10;
}

static inline pte_t *arch_pte_to_ptr(uint64_t pte) {
    return (pte_t *)(uintptr_t)(arch_pte_addr(pte) + PAGE_OFFSET);
}

static inline uint64_t arch_make_satp(void *pgdir) {
    return (1UL << 31) | ((((uint32_t)(uintptr_t)pgdir - PAGE_OFFSET)) >> 12);
}

static inline uint64_t arch_make_addr_space_token(void *pgdir) {
#ifdef CONFIG_NOMMU
    (void)pgdir;
    return 0;
#else
    return arch_make_satp(pgdir);
#endif
}

#define ARCH_PTE_PPN_MASK 0x3FFUL
#define arch_pte_flags(pte) ((uint32_t)(pte) & ARCH_PTE_PPN_MASK)

static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    return arch_pte_from_pa(pa) | flags | PTE_V;
}

#endif
