#ifndef _ARCH_LOONGARCH64_MM_H
#define _ARCH_LOONGARCH64_MM_H

#include "types.h"

/* LoongArch 3-level page table (similar layout to SV39 for software walk) */
#define ARCH_PT_LEVELS    3
#define ARCH_PT_ROOT_LEVEL 2
#define ARCH_PT_BITS      9
#define ARCH_PT_ENTRIES   512
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END  512

/*
 * LoongArch hardware PTE format (leaf):
 *   [0]     V  - Valid
 *   [1]     D  - Dirty / Write-enable
 *   [3:2]   PLV - Privilege level (0=kern, 3=user)
 *   [5:4]   MAT - Memory access type (1=coherent cached)
 *   [6]     G  - Global
 *   [7]     LEAF - Software: set on leaf entries for arch_pte_is_leaf()
 *   [8]     COW - Software: copy-on-write marker
 *   [61:12] PPN
 *
 * We define semantic flags (PTE_R/W/X/U) that map to hw bits so
 * existing code using PTE_R|PTE_W checks continues to work.
 */
/*
 * LoongArch hardware PTE format (leaf entry):
 *   [0]     V  - Valid
 *   [1]     D  - Dirty (write-enabled)
 *   [3:2]   PLV - Privilege level (0=kern, 3=user)
 *   [5:4]   MAT - Memory access type (1=coherent cached)
 *   [6]     G  - Global
 *   [7]     SOFT0 - Software use: leaf marker
 *   [8]     SOFT1 - Software use: COW marker
 *   [61:12] PPN - Physical page number
 *
 * Non-leaf entries: only V and PPN matter.
 *
 * Semantic aliases (PTE_R/W/X/U) are mapped to hw bits so that
 * generic code using PTE_R|PTE_W checks continues to work.
 */
#define PTE_V    (1UL << 0)
#define PTE_D    (1UL << 1)
#define PTE_R    (1UL << 0)   /* Read = Valid */
#define PTE_W    (1UL << 1)   /* Write = Dirty */
#define PTE_X    (1UL << 0)   /* Exec = Valid */
#define PTE_U    (3UL << 2)   /* User = PLV3 */
#define PTE_G    (1UL << 6)
#define PTE_A    (0UL)        /* Accessed: no hw bit on LoongArch */
#define PTE_COW  (1UL << 8)   /* COW software marker */
#define PTE_LEAF (1UL << 7)   /* Software leaf marker */

#define PTE_PLV0 (0UL << 2)
#define PTE_PLV3 (3UL << 2)
#define PTE_MAT1 (1UL << 4)   /* Coherent cached */

/* Kernel leaf: V + D + PLV0 + MAT1 + leaf marker */
#define PTE_KERN (PTE_V | PTE_D | PTE_PLV0 | PTE_MAT1 | PTE_LEAF)
/* User leaf: V + D + PLV3 + MAT1 + leaf marker */
#define PTE_USER (PTE_V | PTE_D | PTE_PLV3 | PTE_MAT1 | PTE_LEAF)

/* Non-leaf (page directory) entry: just V + PPN */
#define PTE_DIR  (PTE_V)

static inline int arch_pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) && (pte & PTE_LEAF);
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    return (int)((va >> (12 + 9 * level)) & 0x1FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return (pte >> 12) & 0x0FFFFFFFFFFFFFUL;
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

#endif
