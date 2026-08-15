#ifndef _ARCH_LOONGARCH32_MM_H
#define _ARCH_LOONGARCH32_MM_H

#include "core/types.h"

/*
 * LoongArch32 2-level page table (4K pages).
 *
 * The NaiLoong Core has no hardware page-table walker (no lddir/ldpte and no
 * PWCL/PWCH); the TLB-refill exception handler performs a fully software
 * walk (boot/entry.S + trap/trap.S).  Both levels are 1024 entries of 4
 * bytes:
 *   level 1 (root): index = va[31:22]
 *   level 0 (leaf): index = va[21:12]
 *
 * In-memory PTE layout (32-bit):
 *   [31:12] PPN (pa[31:12])
 *   [11]    software LEAF
 *   [10]    software COW
 *   [9]     software X
 *   [8]     software W
 *   [7]     software R
 *   [6]     G       global
 *   [5:4]   MAT     memory access type (01 = CC, cached)
 *   [3:2]   PLV     privilege (0 kernel / 3 user)
 *   [1]     D       dirty
 *   [0]     V       valid
 *
 * The hardware TLBELO register uses the same bit positions for G/MAT/PLV/D/V
 * and holds PPN at [27:8]; the refill handler maps between the two.
 */
#define ARCH_PT_LEVELS    2
#define ARCH_PT_ROOT_LEVEL 1
#define ARCH_PT_BITS      10
#define ARCH_PT_ENTRIES   1024
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END  512

#define LA_PTE_VALID    (1UL << 0)
#define LA_PTE_DIRTY    (1UL << 1)
#define LA_PTE_PLV0     (0UL << 2)
#define LA_PTE_PLV3     (3UL << 2)
#define LA_PTE_MAT1     (1UL << 4)
#define LA_PTE_GLOBAL   (1UL << 6)

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
    return (pte & PTE_V) &&
           ((pte & PTE_LEAF) ||
            (pte & (PTE_R | PTE_W | PTE_X | PTE_U |
                    PTE_D | PTE_COW | PTE_MAT1)));
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    return (int)((va >> (12 + 10 * level)) & 0x3FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return (uint32_t)(pte >> 12) & 0xFFFFFUL;
}

static inline uint64_t arch_pte_addr(uint64_t pte) {
    return arch_pte_ppn(pte) << 12;
}

static inline uint64_t arch_pte_from_pa(uint64_t pa) {
    return (uint32_t)pa & 0xFFFFF000UL;
}

static inline pte_t *arch_pte_to_ptr(uint64_t pte) {
    return (pte_t *)(uintptr_t)(arch_pte_addr(pte) + PAGE_OFFSET);
}

static inline uint64_t arch_make_satp(void *pgdir) {
    return (uint64_t)((uint32_t)(uintptr_t)pgdir - PAGE_OFFSET);
}

static inline uint64_t arch_make_addr_space_token(void *pgdir) {
    return arch_make_satp(pgdir);
}

#define ARCH_PTE_PPN_MASK  0xFFFFF000UL
#define arch_pte_flags(pte) ((uint32_t)(pte) & ARCH_PTE_PPN_MASK)

static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    return arch_pte_from_pa(pa) |
           (flags & (PTE_R | PTE_W | PTE_X | PTE_U |
                     PTE_G | PTE_A | PTE_D | PTE_COW |
                     PTE_LEAF | PTE_MAT1)) |
           LA_PTE_VALID;
}

#endif
