#ifndef _ARCH_X86_64_MM_H
#define _ARCH_X86_64_MM_H

#include "core/types.h"
#ifdef CONFIG_SWAP
#include "mm/swap.h"
#endif

/* x86_64 4-level page table constants */
#define ARCH_PT_LEVELS    4
#define ARCH_PT_ROOT_LEVEL 3
#define ARCH_PT_BITS      9
#define ARCH_PT_ENTRIES   512
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END  256

/* PTE flag bits (x86_64 hardware format) */
#define PTE_V    (1UL << 0)   /* Present */
#define PTE_R    (1UL << 0)   /* Present implies readable */
#define PTE_W    (1UL << 1)   /* Read/Write */
#define PTE_U    (1UL << 2)   /* User/Supervisor */
#define PTE_X    (1UL << 11)  /* Software: executable; arch_pte_leaf clears NX */
#define PTE_G    (1UL << 8)   /* Global */
#define PTE_A    (1UL << 5)   /* Accessed */
#define PTE_D    (1UL << 6)   /* Dirty */
#define PTE_COW  (1UL << 9)   /* Software: copy-on-write */
#define PTE_LEAF (1UL << 10)  /* Software: 4K page leaf marker */
#define PTE_NX   (1UL << 63)  /* No-execute */
#define PTE_PS   (1UL << 7)   /* Page Size (huge page) */

#ifdef CONFIG_SWAP
#define PTE_SWAP PTE_LEAF

static inline int pte_present(uint64_t pte) {
    return (pte & PTE_V) != 0;
}

static inline int pte_is_swap(uint64_t pte) {
    return !pte_present(pte) && (pte & PTE_SWAP) != 0;
}

static inline swap_entry_t pte_to_swp_entry(uint64_t pte) {
    return (pte >> 12) & ((1ULL << (SWP_TYPE_BITS + SWP_OFFSET_BITS)) - 1);
}

static inline uint64_t swp_entry_to_pte(swap_entry_t entry) {
    return PTE_SWAP | (entry << 12);
}
#endif

/* Compat aliases for arch-agnostic code */
#define PTE_MAT1  0UL

#define PTE_KERN (PTE_V | PTE_W | PTE_NX | PTE_LEAF)
#define PTE_USER (PTE_V | PTE_W | PTE_U | PTE_NX | PTE_LEAF)
#define PTE_DIR  (PTE_V | PTE_W | PTE_U)

static inline int arch_pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) && (pte & (PTE_LEAF | PTE_PS));
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    return (int)((va >> (12 + 9 * level)) & 0x1FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return (pte >> 12) & ((1UL << 40) - 1);
}

static inline uint64_t arch_pte_addr(uint64_t pte) {
    return arch_pte_ppn(pte) << 12;
}

static inline uint64_t arch_pte_from_pa(uint64_t pa) {
    return pa & ~0xFFFUL;
}

static inline uint64_t *arch_pte_to_ptr(uint64_t pte) {
    return (uint64_t *)(arch_pte_addr(pte) + PAGE_OFFSET);
}

static inline uint64_t arch_make_satp(void *pgdir) {
    /* CR3 expects physical address of PML4 */
    return (uint64_t)(uintptr_t)pgdir - PAGE_OFFSET;
}

static inline uint64_t arch_make_addr_space_token(void *pgdir) {
    return arch_make_satp(pgdir);
}

static inline uint64_t arch_pte_flags(uint64_t pte) {
    uint64_t flags = pte & 0xFFFUL;
    if (!(pte & PTE_NX))
        flags |= PTE_X;
    return flags;
}

static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    uint64_t pte = (pa & ~0xFFFUL) | (flags & 0xFFFUL) | PTE_V | PTE_LEAF;
    if (!(flags & PTE_X))
        pte |= PTE_NX;
    return pte;
}

#endif
