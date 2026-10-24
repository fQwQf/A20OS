/* Host check for the real x86 page-table header and block encoder. */
#include <stdio.h>

#ifndef PAGE_OFFSET
#define PAGE_OFFSET 0xffff800000000000UL
#endif
#include "page_table.h"
#ifndef ARCH_PTE_BLOCK_ONLY_FLAGS
#define ARCH_PTE_BLOCK_ONLY_FLAGS PTE_PS
#endif

#ifdef OLD_NEGATIVE_CONTROL
/* Model the old x86 fallback: generic huge paths called arch_pte_leaf(). */
#undef arch_pte_block
#define arch_pte_block(pa, flags) arch_pte_leaf((pa), (flags))
#endif

#define PMD_SIZE (1UL << 21)
#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (pte=%016llx)\n", label, \
                (unsigned long long)pte); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const paddr_t pa = (paddr_t)0x40000000UL;
    const uint64_t rwx_flags = PTE_U | PTE_W | PTE_A | PTE_D | PTE_COW | PTE_G | PTE_X;
    uint64_t pte = arch_pte_block(pa, rwx_flags);

    CHECK((pa & (PMD_SIZE - 1)) == 0, "test PA is PMD aligned");
    CHECK((pte & PTE_PS) != 0, "PMD block sets hardware PS");
    CHECK((pte & PTE_LEAF) == 0, "PMD block clears software 4K marker");
    CHECK((pte & (PTE_U | PTE_W | PTE_A | PTE_D | PTE_COW | PTE_G)) ==
          (PTE_U | PTE_W | PTE_A | PTE_D | PTE_COW | PTE_G),
          "block preserves U/RW/A/D/COW/G");
    CHECK((pte & PTE_NX) == 0, "executable block clears NX");
    CHECK(arch_pte_addr(pte) == pa, "block retains physical base");

    /* The COW parent downgrade rebuilds the same level-1 block with W clear. */
    uint64_t cow_flags = arch_pte_flags(pte);
    cow_flags = (cow_flags & ~PTE_W) | PTE_COW;
    pte = arch_pte_block(pa, cow_flags);
    CHECK((pte & PTE_PS) && !(pte & PTE_W) && (pte & PTE_COW),
          "fork downgrade keeps block and write-protects it");
    CHECK(!(pte & PTE_NX), "fork downgrade retains executable permission");

    /* A block-level permission update through this encoder retains PS. The
     * actual mprotect transaction demotes first and then writes 4K leaves. */
    uint64_t protect_flags = arch_pte_flags(pte) & ~(PTE_W | PTE_X);
    pte = arch_pte_block(pa, protect_flags);
    CHECK((pte & PTE_PS) && !(pte & PTE_W) && (pte & PTE_NX),
          "block permission replacement keeps PS and applies NX");

    /* Fault COW copy/reuse replacements use this encoder for PMD-sized leaves. */
    pte = arch_pte_block(pa + PMD_SIZE, PTE_U | PTE_W | PTE_X);
    CHECK((pte & PTE_PS) && (pte & PTE_W) && !(pte & PTE_NX),
          "writable executable COW replacement is a block");
    pte = arch_pte_block(pa, PTE_U | PTE_COW);
    CHECK((pte & PTE_PS) && !(pte & PTE_W) && (pte & PTE_NX),
          "reused COW mapping is a protected block");

    uint64_t demoted_flags = protect_flags & ~ARCH_PTE_BLOCK_ONLY_FLAGS;
    pte = arch_pte_leaf(pa, demoted_flags);
    CHECK(!(pte & PTE_PS) && (pte & PTE_LEAF),
          "mprotect-demoted base page remains a 4K leaf, not PS");
    CHECK(!(pte & PTE_W) && (pte & PTE_NX),
          "demoted read-only page keeps protection flags");

    pte = arch_pte_leaf(pa, PTE_U | PTE_W | PTE_X);
    CHECK(!(pte & PTE_PS) && (pte & PTE_LEAF) &&
          (pte & PTE_W) && !(pte & PTE_NX),
          "ordinary base page keeps RWX and PS clear");

    puts("x86 pte block host checks passed");
    return 0;
}
