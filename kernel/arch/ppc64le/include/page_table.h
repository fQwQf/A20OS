#ifndef _ARCH_PPC64LE_MM_H
#define _ARCH_PPC64LE_MM_H

#include "core/types.h"

#ifndef PAGE_SHIFT
#define PAGE_SHIFT         12
#endif
#ifndef PAGE_MASK
#define PAGE_MASK          (~(PAGE_SIZE - 1UL))
#endif

#define ARCH_PT_LEVELS     4
#define ARCH_PT_ROOT_LEVEL 3
#define ARCH_PT_BITS       9
#define ARCH_PT_ENTRIES    512
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END   256

#define PTE_V              (1UL << 0)
#define PTE_R              (1UL << 55)
#define PTE_W              (1UL << 56)
#define PTE_X              (1UL << 57)
#define PTE_U              (1UL << 58)
#define PTE_G              (1UL << 59)
#define PTE_A              (1UL << 60)
#define PTE_D              (1UL << 61)
#define PTE_COW            (1UL << 62)
#define PTE_LEAF           (1UL << 63)
#define PTE_MAT1           (1UL << 54)

#define PTE_KERN           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_G | PTE_MAT1 | PTE_LEAF)
#define PTE_USER           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF)
#define PTE_DIR            (PTE_V)

static inline int arch_pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) && (pte & PTE_LEAF);
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    return (int)((va >> (PAGE_SHIFT + ARCH_PT_BITS * level)) & 0x1FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return (pte >> PAGE_SHIFT) & 0x0000FFFFFFFFFULL;
}

static inline uint64_t arch_pte_addr(uint64_t pte) {
    return arch_pte_ppn(pte) << PAGE_SHIFT;
}

static inline uint64_t arch_pte_from_pa(uint64_t pa) {
    return pa & 0x0000FFFFFFFFF000UL;
}

static inline uint64_t *arch_pte_to_ptr(uint64_t pte) {
    return (uint64_t *)(uintptr_t)(arch_pte_addr(pte) + PAGE_OFFSET);
}

static inline uint64_t arch_make_satp(void *pgdir) {
    return (uint64_t)(uintptr_t)pgdir - PAGE_OFFSET;
}

static inline uint64_t arch_make_addr_space_token(void *pgdir) {
    return arch_make_satp(pgdir);
}

#define ARCH_PTE_PPN_MASK  0x0000FFFFFFFFF000UL
#define arch_pte_flags(pte) ((pte) & ~ARCH_PTE_PPN_MASK)

static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    return arch_pte_from_pa(pa) | PTE_V |
           (flags & (PTE_R | PTE_W | PTE_X | PTE_U |
                     PTE_G | PTE_A | PTE_D | PTE_COW |
                     PTE_LEAF | PTE_MAT1));
}

#endif
