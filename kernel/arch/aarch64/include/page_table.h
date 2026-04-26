#ifndef _ARCH_AARCH64_MM_H
#define _ARCH_AARCH64_MM_H

#include "core/types.h"

/*
 * AArch64 stage-1 translation, 4KB granule, 48-bit VA, 4 levels.
 *
 * Generic MM code expects semantic PTE_{R,W,X,U,...} bits to be directly
 * queryable and mutable.  We therefore keep those semantics in software bits
 * and synthesize the hardware AP/UXN/PXN/AttrIndx fields when writing a leaf.
 *
 * Non-leaf table entries are emitted by generic code as:
 *   arch_pte_from_pa(next) | PTE_V
 *
 * so PTE_V must already encode a valid table descriptor.
 */
#define ARCH_PT_LEVELS     4
#define ARCH_PT_ROOT_LEVEL 3
#define ARCH_PT_BITS       9
#define ARCH_PT_ENTRIES    512
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END   1

#define A64_DESC_VALID     (1UL << 0)
#define A64_DESC_TABLE     (1UL << 1)
#define A64_ATTRINDX_SHIFT 2
#define A64_AP_SHIFT       6
#define A64_SH_SHIFT       8
#define A64_AF             (1UL << 10)
#define A64_nG             (1UL << 11)
#define A64_PXN            (1UL << 53)
#define A64_UXN            (1UL << 54)

/* Software semantic bits. */
#define PTE_V              (A64_DESC_VALID | A64_DESC_TABLE)
#define PTE_R              (1UL << 55)
#define PTE_W              (1UL << 56)
#define PTE_X              (1UL << 57)
#define PTE_U              (1UL << 58)
#define PTE_G              (1UL << 59)
#define PTE_A              (1UL << 60)
#define PTE_D              (1UL << 61)
#define PTE_COW            (1UL << 62)
#define PTE_LEAF           (1UL << 63)

#define PTE_MAT1           (1UL << 52)

#define PTE_KERN           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_G | PTE_MAT1 | PTE_LEAF)
#define PTE_USER           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF)

static inline int arch_pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) && (pte & PTE_LEAF);
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    return (int)((va >> (12 + 9 * level)) & 0x1FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return (pte >> 12) & 0x0000FFFFFFFFFULL;
}

static inline uint64_t arch_pte_addr(uint64_t pte) {
    return arch_pte_ppn(pte) << 12;
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

#define ARCH_PTE_PPN_MASK  0x0000FFFFFFFFF000UL
#define arch_pte_flags(pte) ((pte) & ~ARCH_PTE_PPN_MASK)

static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    uint64_t pte = arch_pte_from_pa(pa) | PTE_V |
                   (flags & (PTE_R | PTE_W | PTE_X | PTE_U |
                             PTE_G | PTE_A | PTE_D | PTE_COW |
                             PTE_LEAF | PTE_MAT1));
    uint64_t ap;

    if (flags & PTE_U)
        ap = (flags & PTE_W) ? 0x1UL : 0x3UL;
    else
        ap = (flags & PTE_W) ? 0x0UL : 0x2UL;

    pte |= A64_AF;
    pte |= (ap << A64_AP_SHIFT);
    pte |= ((flags & PTE_MAT1) ? 0x1UL : 0x0UL) << A64_ATTRINDX_SHIFT;
    if (flags & PTE_U)
        pte |= A64_nG;
    if (flags & PTE_MAT1)
        pte |= (0x3UL << A64_SH_SHIFT);
    if (!(flags & PTE_X))
        pte |= A64_UXN | A64_PXN;
    else if (flags & PTE_U)
        pte |= A64_PXN;
    else
        pte |= A64_UXN;
    return pte;
}

#endif
