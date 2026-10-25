/* Metadata for native stage-2 on a host without the shared stage-1 engine.
 * The descriptor shape is identical; no host page-table walker is enabled. */
#include "hyp/hyp.h"
#include "mm/frame.h"
#include "core/string.h"
#if defined(ARCH_HAS_HYP_S2) && !defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
_Static_assert(sizeof(pt_meta_t) <= PAGE_SIZE, "stage-2 metadata fits one page");
pt_meta_t *mm_pt_meta(pte_t *table)
{
    pfn_t p = virt_to_pfn(table);
    if (!pfn_valid(p) || pfa.meta[p].flags != FRAME_F_PT) return NULL;
    return pfa.meta[p].pt;
}
int mm_pt_node_init_atomic(pte_t *table, int level)
{
    pfn_t p = virt_to_pfn(table), m = pfa_alloc_flags(0, 0);
    if (!pfn_valid(p) || m == PFN_NONE) return -ENOMEM;
    pt_meta_t *meta = pfn_to_virt(m);
    memset(meta, 0, PAGE_SIZE);
    meta->level = level;
    uint64_t irq = spin_lock_irqsave(&pfa.lock);
    pfa.meta[p].pt = meta;
    pfa.meta[p].flags = FRAME_F_PT;
    spin_unlock_irqrestore(&pfa.lock, irq);
    return 0;
}
void mm_pt_node_fini(pte_t *table)
{
    pfn_t p = virt_to_pfn(table);
    pt_meta_t *m = mm_pt_meta(table);
    if (!m) return;
    uint64_t irq = spin_lock_irqsave(&pfa.lock);
    pfa.meta[p].pt = NULL;
    pfa.meta[p].flags = FRAME_F_ALLOC;
    spin_unlock_irqrestore(&pfa.lock, irq);
    pfa_free(virt_to_pfn(m), 0);
}
void mm_pt_note_present(pte_t *table, int level, int idx, uint8_t cls)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    if (!m || idx < 0 || idx >= MM_PT_META_ENTRIES) return;
    if (!m->cls[idx]) m->nr_present++;
    m->cls[idx] = cls;
}
void mm_pt_note_absent(pte_t *table, int level, int idx)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    if (!m || idx < 0 || idx >= MM_PT_META_ENTRIES) return;
    if (m->cls[idx] && m->nr_present) m->nr_present--;
    m->cls[idx] = 0;
}
void mm_pt_frame_return(pfn_t p)
{
    if (!pfn_valid(p)) return;
    uint64_t irq = spin_lock_irqsave(&pfa.lock);
    pfa.meta[p].flags &= ~FRAME_F_GUEST;
    spin_unlock_irqrestore(&pfa.lock, irq);
}
int frame_is_lent_to_guest(pfn_t p)
{
    return pfn_valid(p) && (pfa.meta[p].flags & FRAME_F_GUEST);
}
#endif
