#ifndef _ARCH_AARCH64_MM_H
#define _ARCH_AARCH64_MM_H

#include "core/types.h"
#ifdef CONFIG_SWAP
#include "mm/swap.h"
#endif

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

/*
 * Keep software state out of architecturally meaningful descriptor bits.
 * In particular bit 52 is the Contiguous hint, not a software MAT bit: setting
 * it on independently allocated pages lets hardware combine unrelated PTEs.
 * AArch64 has no read-disable permission, so a valid leaf is always readable;
 * dirty is tracked conservatively as writable on systems without HAFDBS.
 */
#define PTE_V              A64_DESC_VALID
#define PTE_R              PTE_V
#define PTE_LEAF           (1UL << 55)
#define PTE_W              (1UL << 56)
#define PTE_X              (1UL << 57)
#define PTE_COW            (1UL << 58)
#define PTE_U              A64_nG
#define PTE_G              0UL
#define PTE_A              A64_AF
#define PTE_D              PTE_W

#ifdef CONFIG_SWAP
#define PTE_SWAP            PTE_LEAF

static inline int pte_present(uint64_t pte) {
    return (pte & PTE_V) != 0;
}

static inline int pte_is_swap(uint64_t pte) {
    return !pte_present(pte) && (pte & PTE_SWAP) != 0;
}

static inline int pte_none(uint64_t pte) {
    return pte == 0;
}

static inline uint64_t swp_entry_to_pte(swap_entry_t entry) {
    return PTE_SWAP | (entry << 12);
}

static inline swap_entry_t pte_to_swp_entry(uint64_t pte) {
    return (pte >> 12) & ((1ULL << (SWP_TYPE_BITS + SWP_OFFSET_BITS)) - 1);
}
#endif

#define PTE_MAT1           (1UL << A64_ATTRINDX_SHIFT)

_Static_assert((PTE_MAT1 & (1UL << 52)) == 0,
               "AArch64 bit 52 is the hardware Contiguous hint");

#define PTE_KERN           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_G | PTE_MAT1 | PTE_LEAF)
#define PTE_USER           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF)
#define PTE_DIR            (PTE_V | A64_DESC_TABLE)
#define ARCH_HAS_PTE_BLOCK 1

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

static inline uint64_t arch_make_addr_space_token(void *pgdir) {
#ifdef CONFIG_NOMMU
    (void)pgdir;
    return 0;
#else
    return arch_make_satp(pgdir);
#endif
}

#define ARCH_PTE_PPN_MASK  0x0000FFFFFFFFF000UL
_Static_assert(((PTE_LEAF | PTE_W | PTE_X | PTE_COW) &
                ARCH_PTE_PPN_MASK) == 0,
               "AArch64 software PTE flags overlap the output address");
#define A64_SEMANTIC_FLAGS (PTE_V | PTE_W | PTE_X | PTE_U | PTE_A | \
                            PTE_COW | PTE_LEAF | PTE_MAT1)
#define arch_pte_flags(pte) ((pte) & A64_SEMANTIC_FLAGS)

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
    pte |= A64_DESC_TABLE;
    pte |= (ap << A64_AP_SHIFT);
    pte |= flags & PTE_MAT1;
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

static inline uint64_t arch_pte_block(paddr_t pa, uint64_t flags) {
    uint64_t pte = arch_pte_leaf(pa, flags);
    return pte & ~A64_DESC_TABLE;
}

#endif
