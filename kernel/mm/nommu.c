#ifdef CONFIG_NOMMU

#include "mm/mm.h"
#include "mm/frame.h"
#include "core/types.h"
#include "core/consts.h"

/* No page tables under CONFIG_NOMMU, so there is never anything to retire or
 * drain.  Stubs keep the API uniform: the real definitions live inside pt.c's
 * ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU region, but callers such as
 * mm_destroy() are not guarded. */
void mm_pt_retire_table(struct mm_struct *mm, pte_t *table, int level)
{
    (void)mm; (void)table; (void)level;
}

void mm_pt_retire_drain(struct mm_struct *mm) { (void)mm; }

/* Likewise: without page tables there are no per-entry safety bits to set, but
 * mseal() and the userfaultfd register/unregister paths are not guarded and do
 * call this. */
int mm_pt_set_safe_range(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                         unsigned flags, int set)
{
    (void)mm; (void)start; (void)end; (void)flags; (void)set;
    return 0;
}

pte_t *pt_create(void) { return (pte_t *)1; }
void pt_destroy(pt_root_t *pgdir) { (void)pgdir; }
int pt_map(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags) { (void)pgdir; (void)va; (void)pa; (void)flags; return 0; }
/* Signature per mm.h: the huge install takes the mm and the status class the
 * leaf carries, and no page table means neither is used.  A stub still has to
 * match the declaration it is defining -- a stale pt_root_t/4-argument form
 * here is a conflicting-types error, not a silent no-op. */
int pt_map_huge(struct mm_struct *mm, vaddr_t va, paddr_t pa, pte_t flags,
                uint8_t cls) { (void)mm; (void)va; (void)pa; (void)flags; (void)cls; return 0; }
int pt_unmap(struct mm_struct *mm, vaddr_t va) { (void)mm; (void)va; return 0; }
int pt_unmap_leaf(struct mm_struct *mm, vaddr_t va, paddr_t *pa_out, vaddr_t *base_out, size_t *size_out, int *level_out) {
    (void)mm;
    if (pa_out) *pa_out = va;
    if (base_out) *base_out = va;
    if (size_out) *size_out = PAGE_SIZE;
    if (level_out) *level_out = 0;
    return 1;
}
paddr_t pt_translate(pt_root_t *pgdir, vaddr_t va) { (void)pgdir; return va; }
pte_t *pt_walk(pt_root_t *pgdir, vaddr_t va, int alloc) { (void)pgdir; (void)va; (void)alloc; return (pte_t *)1; }
pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t va, int *level_out, vaddr_t *base_out, size_t *size_out) {
    (void)pgdir;
    if (level_out) *level_out = 0;
    if (base_out) *base_out = va;
    if (size_out) *size_out = PAGE_SIZE;
    return (pte_t *)1;
}
int mm_query_leaf(pt_root_t *pgdir, vaddr_t va, mm_leaf_info_t *out) {
    (void)pgdir;
    if (out) {
        out->level = 0;
        out->base = va;
        out->size = PAGE_SIZE;
        out->pa = va;
        out->flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_U;
        out->dirty = 1;
    }
    return 1;
}
uint64_t mm_pagemap_entry(pt_root_t *pgdir, vaddr_t va) { (void)pgdir; return (1ULL << 63) | (va / PAGE_SIZE); }
int mm_query_leaf_kaddr(pt_root_t *pgdir, vaddr_t va, void **kaddr_out, size_t *avail_out) {
    (void)pgdir;
    if (kaddr_out) *kaddr_out = (void *)va;
    if (avail_out) *avail_out = PAGE_SIZE;
    return 1;
}
int mm_fetch_user_insn32(pt_root_t *pgdir, vaddr_t va, uint32_t *out) {
    (void)pgdir;
    if (out) *out = *(uint32_t *)va;
    return 1;
}
int mm_mark_leaf_dirty_if_writable(pt_root_t *pgdir, vaddr_t va) { (void)pgdir; (void)va; return 0; }
int mm_debug_pte_value(pt_root_t *pgdir, vaddr_t va, uintptr_t *slot_out, pte_t *value_out) {
    (void)pgdir;
    (void)va;
    if (slot_out) *slot_out = 1;
    if (value_out) *value_out = 1;
    return 1;
}
void pt_map_kernel(pt_root_t *pgdir) { (void)pgdir; }
int pt_map_range(pt_root_t *pgdir, vaddr_t va, paddr_t pa, size_t size, pte_t flags) {
    (void)pgdir; (void)va; (void)pa; (void)size; (void)flags; return 0;
}
pte_t *pt_clone(pt_root_t *src_pgdir) { (void)src_pgdir; return (pte_t *)1; }
void pt_destroy_user(pt_root_t *pgdir) { (void)pgdir; }

#endif /* CONFIG_NOMMU */
