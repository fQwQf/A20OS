#ifndef _ARCH_ARM32_PAGE_TABLE_H
#define _ARCH_ARM32_PAGE_TABLE_H

#include "core/types.h"
#ifdef CONFIG_SWAP
#include "mm/swap.h"
#endif

/*
 * ARMv7-A short-descriptor translation tables.
 *
 * L1: 4096 entries, 16 KiB, 1 MiB sections or L2 table pointers.
 * L2: 256 entries, 1 KiB, 4 KiB small pages.
 *
 * The generic MM code indexes tables through arch_pt_vpn(), which is
 * overridden below because the two ARM levels use different index widths
 * (12 bits at L1, 8 bits at L2).
 */
#define ARCH_PT_LEVELS     2
#define ARCH_PT_ROOT_LEVEL 1
#define ARCH_PT_BITS       8
#define ARCH_PT_ENTRIES    4096
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END   0x700 /* VA 0x00000000 .. 0x70000000 */
#define ARCH_PT_LEVEL_ENTRIES(level) \
    ((level) == ARCH_PT_ROOT_LEVEL ? ARCH_PT_ENTRIES : 256)

/*
 * The short-descriptor setup currently keeps domains in manager mode, so AP
 * write protection cannot provide reliable copy-on-write faults.  Fork must
 * therefore give private mappings distinct physical pages.
 */
#define ARCH_FORK_REQUIRES_PRIVATE_COPY 1

/* Match the ARM32 section size (1 MiB) to the generic huge-page helpers. */
#undef  PMD_SHIFT
#undef  PMD_SIZE
#undef  PMD_ORDER
#undef  PMD_PAGE_COUNT
#define PMD_SHIFT          20
#define PMD_SIZE           (1UL << PMD_SHIFT)
#define PMD_ORDER          (PMD_SHIFT - PAGE_SIZE_BITS)
#define PMD_PAGE_COUNT     (PMD_SIZE / PAGE_SIZE)

/* Hardware descriptor type bits. */
#define ARM32_L1_TYPE_TABLE  0x1U
#define ARM32_L1_TYPE_SECTION 0x2U
#define ARM32_L2_TYPE_SMALL  0x2U

/* Hardware attribute bits used in leaf descriptors. */
#define ARM32_PTE_B          (1U << 2)
#define ARM32_PTE_C          (1U << 3)
#define ARM32_PTE_AP0        (1U << 4)  /* small page AP[0] */
#define ARM32_PTE_AP1        (1U << 5)  /* small page AP[1] */
#define ARM32_PTE_TEX0       (1U << 6)

/*
 * Software PTE bits.
 *
 * PTE_V lives in bit 9.  At L1 this is IMPLEMENTATION DEFINED; at L2 it
 * is AP[2], but the kernel runs with all domains in manager mode so the
 * hardware access-permission checks are bypassed.  PTE_LEAF is bit 1, the
 * hardware leaf type bit (0b10) shared by L1 sections and L2 small pages.
 */
#define PTE_V                (1U << 9)
#define PTE_LEAF             (1U << 1)

/*
 * Semantic permission/attribute bits.  These are stored in positions that
 * are harmless for both descriptor levels:
 *   - PTE_W uses bit 10 (small page S, section AP[0]).
 *   - PTE_U uses bit 11 (small page nG, section AP[1]).
 *   - PTE_COW uses bit 6  (small page TEX[0], section Domain[1]).
 *   - PTE_MAT1 uses bit 3 (small page C, section C).
 * PTE_R is aliased to PTE_V because any valid mapping is readable.
 */
#define PTE_R                PTE_V
#define PTE_W                (1U << 10)
#define PTE_U                (1U << 11)
#define PTE_X                0UL
#define PTE_G                0UL
#define PTE_A                0UL
#define PTE_D                0UL
#define PTE_COW              (1U << 6)
#define PTE_MAT1             ARM32_PTE_C

#ifdef CONFIG_SWAP
/* TEX[1] is unused by every descriptor emitted by this kernel. */
#define PTE_SWAP             (1U << 7)

static inline int pte_present(uint32_t pte) {
    return (pte & PTE_V) != 0;
}

static inline int pte_is_swap(uint32_t pte) {
    return !pte_present(pte) && (pte & PTE_SWAP) != 0;
}

static inline swap_entry_t pte_to_swp_entry(uint32_t pte) {
    return ((pte >> 2) & ((1ULL << SWP_TYPE_BITS) - 1)) |
           (((swap_entry_t)pte >> 12) << SWP_TYPE_BITS);
}

static inline uint32_t swp_entry_to_pte(swap_entry_t entry) {
    return PTE_SWAP |
           ((uint32_t)(entry & ((1ULL << SWP_TYPE_BITS) - 1)) << 2) |
           ((uint32_t)(entry >> SWP_TYPE_BITS) << 12);
}
#endif

#define PTE_KERN             (PTE_V | PTE_R | PTE_W | PTE_LEAF | PTE_MAT1)
#define PTE_USER             (PTE_V | PTE_R | PTE_W | PTE_U | PTE_LEAF | PTE_MAT1)
#define PTE_DIR              (PTE_V | ARM32_L1_TYPE_TABLE)

static inline int arch_pte_is_leaf(uint32_t pte) {
    return (pte & 0x3U) == ARM32_L2_TYPE_SMALL;
}

static inline int arch_pt_vpn(uint32_t va, int level) {
    if (level == ARCH_PT_ROOT_LEVEL)
        return (int)((va >> 20) & 0xFFFU);
    return (int)((va >> 12) & 0xFFU);
}

static inline uint32_t arch_pte_ppn(uint32_t pte) {
    return (pte & 0xFFFFF000U) >> 12;
}

static inline uint32_t arch_pte_addr(uint32_t pte) {
    return pte & 0xFFFFF000U;
}

static inline uint32_t arch_pte_from_pa(uint32_t pa) {
    return pa & 0xFFFFF000U;
}

static inline pte_t *arch_pte_to_ptr(uint32_t pte) {
    return (pte_t *)(uintptr_t)(arch_pte_addr(pte) + PAGE_OFFSET);
}

static inline uint32_t arch_make_satp(void *pgdir) {
    return (uint32_t)((uintptr_t)pgdir - PAGE_OFFSET);
}

static inline uint32_t arch_make_addr_space_token(void *pgdir) {
    return arch_make_satp(pgdir);
}

static inline uint32_t arch_pte_flags(uint32_t pte) {
    return pte & 0xFFFU;
}

/* Helper to build an L2 small-page descriptor. */
static inline uint32_t arch_pte_leaf(paddr_t pa, uint32_t flags) {
    uint32_t pte = (pa & 0xFFFFF000U) | ARM32_L2_TYPE_SMALL | PTE_V;

    if (flags & PTE_W) {
        pte |= PTE_W;
        pte |= ARM32_PTE_AP0;
    }
    if (flags & PTE_U) {
        pte |= PTE_U;
        pte |= ARM32_PTE_AP1;
    }
    if (flags & PTE_COW)
        pte |= PTE_COW;
    if (flags & PTE_MAT1)
        pte |= ARM32_PTE_C;

    pte |= ARM32_PTE_B;
    pte |= (flags & PTE_LEAF);
    return pte;
}

#endif
