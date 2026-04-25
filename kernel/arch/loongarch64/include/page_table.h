#ifndef _ARCH_LOONGARCH64_MM_H
#define _ARCH_LOONGARCH64_MM_H

#include "core/types.h"

/* LoongArch 3-level page table (4K pages, software walk matches PWCL/STLBPS) */
#define ARCH_PT_LEVELS    3
#define ARCH_PT_ROOT_LEVEL 2
#define ARCH_PT_BITS      9
#define ARCH_PT_ENTRIES   512
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END  256

/*
 * LoongArch common-page PTE layout:
 *   [0]     V         hardware valid
 *   [1]     D         hardware dirty
 *   [3:2]   PLV       privilege
 *   [5:4]   MAT       memory type
 *   [6]     G         global
 *   [7:11]  ignored by hardware page walk/TLB fill, usable as software bits
 *   [47:12] PPN       physical page number
 *   [61]    NR        no-read
 *   [62]    NX        no-exec
 *
 * Generic MM code expects semantic PTE_R/W/X/U bits to exist and be mutable
 * in-place.  On LoongArch those do not map 1:1 to hardware bits, so we keep
 * R/X/leaf/COW as software bits and synthesize NR/NX/D/W when a leaf PTE is
 * written.
 */
#define LA_PTE_VALID    (1UL << 0)
#define LA_PTE_DIRTY    (1UL << 1)
#define LA_PTE_PLV0     (0UL << 2)
#define LA_PTE_PLV3     (3UL << 2)
#define LA_PTE_MAT1     (1UL << 4)
#define LA_PTE_GLOBAL   (1UL << 6)
#define LA_PTE_NR       (1UL << 61)
#define LA_PTE_NX       (1UL << 62)

/* Generic semantic bits used by the common MM code. */
#define PTE_V           LA_PTE_VALID
#define PTE_D           LA_PTE_DIRTY
#define PTE_R           (1UL << 7)
#define PTE_W           (1UL << 8)
#define PTE_X           (1UL << 9)
#define PTE_COW         (1UL << 10)
#define PTE_LEAF        (1UL << 11)
#define PTE_U           LA_PTE_PLV3
#define PTE_G           LA_PTE_GLOBAL
#define PTE_A           (0UL)          /* Accessed is implicit on LoongArch */
#define PTE_MAT1        LA_PTE_MAT1

#define PTE_PLV0        LA_PTE_PLV0
#define PTE_PLV3        LA_PTE_PLV3

#define PTE_KERN        (PTE_V | PTE_R | PTE_W | PTE_X | PTE_D | PTE_MAT1 | PTE_LEAF)
#define PTE_USER        (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_D | PTE_MAT1 | PTE_LEAF)
#define PTE_DIR         (PTE_V)

static inline int arch_pte_is_leaf(uint64_t pte) {
    /*
     * LoongArch non-leaf entries in this kernel are encoded as plain PTE_V.
     * Real leaf mappings always carry either the explicit software leaf bit
     * or other leaf-only semantic bits (U/R/W/X/D/COW/MAT1).
     *
     * This makes the walker robust against a path that accidentally drops
     * PTE_LEAF: we still classify the entry as a leaf instead of recursing
     * into a user data page as if it were the next-level page table.
     */
    return (pte & PTE_V) &&
           ((pte & PTE_LEAF) ||
            (pte & (PTE_R | PTE_W | PTE_X | PTE_U |
                    PTE_D | PTE_COW | PTE_MAT1)));
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
    return (pa >> 12) << 12;
}

static inline uint64_t *arch_pte_to_ptr(uint64_t pte) {
    return (uint64_t *)(arch_pte_addr(pte) + PAGE_OFFSET);
}

static inline uint64_t arch_make_satp(void *pgdir) {
    return (uint64_t)(uintptr_t)pgdir - PAGE_OFFSET;
}

#define ARCH_PTE_PPN_MASK  0x0000FFFFFFFFF000UL
#define arch_pte_flags(pte) ((pte) & ~ARCH_PTE_PPN_MASK)

static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    uint64_t pte = arch_pte_from_pa(pa) |
                   (flags & (PTE_R | PTE_W | PTE_X | PTE_U |
                             PTE_G | PTE_A | PTE_D | PTE_COW |
                             PTE_LEAF | PTE_MAT1));

    pte |= LA_PTE_VALID;
    if (!(flags & PTE_R)) pte |= LA_PTE_NR;
    if (!(flags & PTE_X)) pte |= LA_PTE_NX;
    return pte;
}

#endif
