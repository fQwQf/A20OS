#ifndef _ARCH_PPC64LE_MM_H
#define _ARCH_PPC64LE_MM_H

#include "core/types.h"
#ifdef CONFIG_SWAP
#include "mm/swap.h"
#endif

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
#define ARCH_PT_ROOT_ENTRIES 8192
#define ARCH_PT_ROOT_ORDER 4
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END   256

/*
 * qemu-system-ppc64 compiles the ppc64 target big-endian, so every physical
 * table load (ldq_phys -> ldq_be_p) reads the process table and the radix
 * page tables BIG-ENDIAN.  The bootstrap (entry.S) therefore installs tables
 * with stdbrx (byte-reversed stores) so the memory bytes are the BE image of
 * the architectural value.
 *
 * The C runtime stores through little-endian stores, so every PTE value must
 * be byte-swapped before storing: the kernel keeps the swap-image of the
 * architectural entry (arch_pte_from_pa swaps the RPN, the PTE_* flags below
 * are positioned so that a bswap maps them onto the PowerISA radix bits that
 * QEMU checks, see target/ppc/mmu-radix64.c):
 *   VALID bit63 <- bswap(PTE_V), LEAF bit62 <- bswap(PTE_LEAF),
 *   RPN bits12-52, R bit8, C bit7, EAA X/RW/R/PRIV bits0-3,
 *   software bits 9-11 (R_PTE_SW1).
 */
#define PTE_V              0x0000000000000080UL
#define PTE_LEAF           0x0000000000000040UL
#define PTE_COW            0x0008000000000000UL
#define PTE_G              0x0004000000000000UL
#define PTE_U              0x0002000000000000UL
#define PTE_D              0x8000000000000000UL
#define PTE_A              0x0001000000000000UL
#define PTE_R              0x0400000000000000UL
#define PTE_W              0x0200000000000000UL
#define PTE_X              0x0100000000000000UL
#define PTE_MAT1           0UL

#ifdef CONFIG_SWAP
/* Bit 1 is not used by the kernel's PPN or flag layout and is outside the
 * payload range when the swap entry is shifted left by 12. */
#define PTE_SWAP           0x0000000000000002UL

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

#define PTE_PRIV           0x0800000000000000UL
#define PTE_KERN           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_G | PTE_MAT1 | PTE_LEAF | PTE_PRIV)
#define PTE_USER           (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF)
/* Non-leaf entry: bswap(VALID | 9), the 9-bit next-level size in R_PDE_NLS. */
#define PTE_DIR            (PTE_V | 0x0900000000000000UL)

#define ARCH_PT_LEVEL_ENTRIES(level) \
    ((level) == ARCH_PT_ROOT_LEVEL ? ARCH_PT_ROOT_ENTRIES : ARCH_PT_ENTRIES)

static inline int arch_pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) && (pte & PTE_LEAF);
}

static inline int arch_pt_vpn(uint64_t va, int level) {
    va &= 0x000FFFFFFFFFFFFFUL;
    if (level == ARCH_PT_ROOT_LEVEL)
        return (int)((va >> 39) & 0x1FFF);
    return (int)((va >> (PAGE_SHIFT + ARCH_PT_BITS * level)) & 0x1FF);
}

static inline uint64_t arch_pte_ppn(uint64_t pte) {
    return (__builtin_bswap64(pte) & 0x01FFFFFFFFFFF000UL) >> PAGE_SHIFT;
}

static inline uint64_t arch_pte_addr(uint64_t pte) {
    return __builtin_bswap64(pte) & 0x01FFFFFFFFFFF000UL;
}

static inline uint64_t arch_pte_from_pa(uint64_t pa) {
    return __builtin_bswap64(pa & 0x01FFFFFFFFFFF000UL);
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

#define ARCH_PTE_PPN_MASK  0x00F0FFFFFFFFFF01UL
#define arch_pte_flags(pte) ((pte) & ~ARCH_PTE_PPN_MASK)

static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    return arch_pte_from_pa(pa) | PTE_V |
           (flags & (PTE_R | PTE_W | PTE_X | PTE_U |
                     PTE_G | PTE_A | PTE_D | PTE_COW |
                     PTE_LEAF | PTE_MAT1 | PTE_PRIV));
}

#endif
