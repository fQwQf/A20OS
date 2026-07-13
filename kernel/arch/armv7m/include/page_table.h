#ifndef _ARCH_ARMV7M_PAGE_TABLE_H
#define _ARCH_ARMV7M_PAGE_TABLE_H

#include "core/types.h"

#define ARCH_PT_LEVELS 1
#define ARCH_PT_ROOT_LEVEL 0
#define ARCH_PT_BITS 0
#define ARCH_PT_ENTRIES 1
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END 1

#define PTE_V    (1UL << 0)
#define PTE_R    (1UL << 1)
#define PTE_W    (1UL << 2)
#define PTE_X    (1UL << 3)
#define PTE_U    (1UL << 4)
#define PTE_G    0UL
#define PTE_A    0UL
#define PTE_D    0UL
#define PTE_COW  0UL
#define PTE_MAT1 0UL
#define PTE_LEAF 0UL
#define PTE_KERN (PTE_V | PTE_R | PTE_W | PTE_X)
#define PTE_USER (PTE_KERN | PTE_U)
#define PTE_DIR  PTE_V

static inline int arch_pte_is_leaf(uint64_t pte) { return (pte & PTE_V) != 0; }
static inline int arch_pt_vpn(uint64_t va, int level) {
    (void)va;
    (void)level;
    return 0;
}
static inline uint64_t arch_pte_addr(uint64_t pte) { return pte & ~0x1FUL; }
static inline uint64_t arch_pte_from_pa(uint64_t pa) { return pa; }
static inline pte_t *arch_pte_to_ptr(uint64_t pte) {
    return (pte_t *)(uintptr_t)arch_pte_addr(pte);
}
static inline uint64_t arch_make_addr_space_token(void *pgdir) {
    (void)pgdir;
    return 0;
}
static inline uint64_t arch_pte_flags(uint64_t pte) { return pte & 0x1FUL; }
static inline uint64_t arch_pte_leaf(paddr_t pa, uint64_t flags) {
    return pa | flags | PTE_V;
}

#endif
