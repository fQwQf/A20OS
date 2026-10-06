#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/pt.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "fs/page_cache.h"
#include "proc/proc.h"
#include "core/string.h"

#ifndef CONFIG_NOMMU

/*
 * Fork / copy-on-write page-table cloning.
 *
 * mm_fork() (in mm/vm.c) drives the VMA snapshot and calls these helpers while
 * holding parent->lock so the parent cannot race a concurrent page fault.
 * The clone walks only already-present leaf PTEs; file-backed shared mappings
 * are left populated on demand.
 */

static size_t vm_pt_level_size(int level) {
    return PAGE_SIZE << (ARCH_PT_BITS * level);
}

static uint64_t mm_cow_flags(uint64_t pte) {
    uint64_t flags = pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_A |
                            PTE_D | PTE_G | PTE_MAT1 | PTE_LEAF |
                            PTE_COW);
    if (pte & (PTE_W | PTE_COW)) {
        flags &= ~(uint64_t)(PTE_W | PTE_D);
        flags |= PTE_COW;
    }
    return flags;
}

/*
 * The status class a page already has, so a caller that rewrites the PTE can
 * hand the class back to mm_pt_sync_status() instead of guessing.
 *
 * The class describes the BACKING (anonymous / file-private / file-shared /
 * VMO), which fork does not change -- it changes sharing, and that is the COW
 * bit, not the class.  So the honest answer is whatever the status already
 * says.  If the status is empty there is nothing to sync against and the
 * caller skips the update, which is why this can fall back to a VMA lookup
 * only to recover a class for pages whose status was never written.
 *
 * The per-PTE status sidecar is a pgtable-ops feature: arm32's short-descriptor
 * backend has none (same split as fault.c's status-driven path), so fork there
 * keeps the PTE-level COW and only skips the status bookkeeping.
 */
#if defined(ARCH_HAS_PGTABLE_OPS)
static uint8_t mm_fork_page_class(struct mm_struct *mm, vaddr_t va)
{
    pte_t *tab = mm_pt_leaf_table(mm->pgdir, va);
    if (tab) {
        uint8_t cur = mm_pt_peek(tab, 0, arch_pt_vpn(va, 0));
        uint8_t cls = MM_ST_GET_CLASS(cur);
        if (cls != MM_ST_INVALID && cls != MM_ST_PT_NODE)
            return cls;
    }
    /* No status to preserve.  Recover the backing from the VMA so a page that
     * predates the cursor still gets a coherent class rather than a guess. */
    mm_seg_t *v = mm_seg_find(mm, va);
    if (!v)
        return MM_ST_ANON_MAPPED;
    if (v->vm_flags & VM_VMO)
        return MM_ST_VMO;
    if (v->vm_flags & VM_FILE)
        return (v->vm_flags & VM_SHARED) ? MM_ST_FILE_SHARED
                                         : MM_ST_FILE_PRIVATE;
    return MM_ST_ANON_MAPPED;
}
#endif /* ARCH_HAS_PGTABLE_OPS */

int mm_fork_clone_page(mm_struct_t *child, mm_struct_t *parent, vaddr_t va,
                       int shared) {
    int level = 0;
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *src = pt_lookup_leaf(parent->pgdir, va, &level, &base, &size);
    if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src) || !(*src & PTE_U))
        return 0;
    if (va != base)
        return 0;

    pte_t *dst = pt_lookup_leaf(child->pgdir, va, NULL, NULL, NULL);
    if (dst && (*dst & PTE_V))
        return 0;

    paddr_t pa = arch_pte_addr(*src);
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -ENOMEM;

    if (!shared && arch_fork_requires_private_copy()) {
        pfn_t copy = pfa_alloc_page();
        if (copy == PFN_NONE)
            return -ENOMEM;
        memcpy(pfn_to_virt(copy), pfn_to_virt(pfn), PAGE_SIZE);
        int r = pt_map(child->pgdir, base, pfn_to_phys(copy),
                       arch_pte_flags(*src));
        if (r < 0) {
            frame_put(copy);
            return r;
        }
        mm_rss_add(child, 1);
        return 0;
    }

    pte_t flags = shared ? arch_pte_flags(*src) : mm_cow_flags(*src);
    frame_get(pfn);

    int r = (level > 0) ? pt_map_huge(child->pgdir, base, pa, flags)
                        : pt_map(child->pgdir, base, pa, flags);
    if (r < 0) {
        frame_put(pfn);
        return r;
    }

    if (!shared && (*src & (PTE_W | PTE_COW))) {
        *src = arch_pte_leaf(pa, flags);
        mm_tlb_note_change(parent, base, size);
        /* The parent's PTE just lost W and gained COW; the status has to
         * follow, or mm_pt_audit_all() reports a prot/cow mismatch and a
         * status-driven fault would keep installing the parent's old
         * write permission over a page the child now shares. */
#if defined(ARCH_HAS_PGTABLE_OPS)
        pte_t *stab = mm_pt_leaf_table(parent->pgdir, base);
        if (stab)
            (void)mm_pt_sync_status(stab, 0, arch_pt_vpn(base, 0),
                                    mm_fork_page_class(parent, base));
#endif /* ARCH_HAS_PGTABLE_OPS */
    }
    mm_rss_add(child, size / PAGE_SIZE);
    return 0;
}

int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                        vaddr_t start, vaddr_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        int r = mm_fork_clone_page(child, parent, va, shared);
        if (r < 0)
            return r;
    }
    return 0;
}

int mm_fork_clone_leaf(mm_struct_t *child, mm_struct_t *parent,
                       pte_t *src_pte, vaddr_t va, int level,
                       int shared) {
    if (!src_pte || !(*src_pte & PTE_V) ||
        !arch_pte_is_leaf(*src_pte) || !(*src_pte & PTE_U))
        return 0;

    paddr_t pa = arch_pte_addr(*src_pte);
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -ENOMEM;

    size_t leaf_size = vm_pt_level_size(level);
    if (!shared && arch_fork_requires_private_copy()) {
        int order = (leaf_size == PMD_SIZE) ? PMD_ORDER : 0;
        if (leaf_size != PAGE_SIZE && leaf_size != PMD_SIZE)
            return -ENOMEM;
        pfn_t copy = pfa_alloc(order);
        if (copy == PFN_NONE)
            return -ENOMEM;
        memcpy(pfn_to_virt(copy), pfn_to_virt(pfn), leaf_size);
        int r = (level > 0) ?
            pt_map_huge(child->pgdir, va, pfn_to_phys(copy),
                        arch_pte_flags(*src_pte)) :
            pt_map(child->pgdir, va, pfn_to_phys(copy),
                   arch_pte_flags(*src_pte));
        if (r < 0) {
            pfa_free(copy, order);
            return r;
        }
        mm_rss_add(child, leaf_size / PAGE_SIZE);
        return 0;
    }

    mm_seg_t *vma = parent ? mm_seg_find(parent, va) : NULL;
    /* VMO frames are owned by the VMO object; mappings never hold frame
     * references (vmo_get_page contract).  Cloning a VMO PTE as shared must
     * not frame_get: the VMA's own vmo reference (vma_ref_fork) keeps the
     * frames alive, and teardown never puts them. */
    int is_vmo = vma && (vma->vm_flags & VM_VMO);
    page_cache_page_t *pcp =
        is_vmo ? NULL : mm_file_cache_mapping_get(vma, va, pfn);

    if (!pcp && !is_vmo)
        frame_get(pfn);

    pte_t flags = shared ? arch_pte_flags(*src_pte) : mm_cow_flags(*src_pte);
    int r = (level > 0) ? pt_map_huge(child->pgdir, va, pa, flags)
                        : pt_map(child->pgdir, va, pa, flags);
    if (r < 0) {
        if (pcp) {
            page_cache_put(pcp);
        } else if (!is_vmo) {
            frame_put(pfn);
        }
        return r;
    }

    if (!shared && (*src_pte & (PTE_W | PTE_COW))) {
        *src_pte = arch_pte_leaf(pa, flags);
        mm_tlb_note_change(parent, va, leaf_size);
        /* See mm_fork_clone_page(): the parent's status must follow the PTE
         * it just rewrote.  Only level-0 leaves have a per-PTE status slot;
         * a large leaf is one entry covering many virtual pages and the
         * auditor checks it as a whole. */
#if defined(ARCH_HAS_PGTABLE_OPS)
        if (level == 0) {
            pte_t *stab = mm_pt_leaf_table(parent->pgdir, va);
            if (stab)
                (void)mm_pt_sync_status(stab, 0, arch_pt_vpn(va, 0),
                                        mm_fork_page_class(parent, va));
        }
#endif /* ARCH_HAS_PGTABLE_OPS */
    }
    mm_rss_add(child, vm_pt_level_size(level) / PAGE_SIZE);
    return 0;
}

int mm_fork_clone_present_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared) {
    if (!table || start >= end)
        return 0;

    size_t span = vm_pt_level_size(level);
    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        vaddr_t entry_base = base + (vaddr_t)i * span;
        vaddr_t entry_end = entry_base + span;
        if (entry_end <= start)
            continue;
        if (entry_base >= end)
            break;

        pte_t *pte = &table[i];
        if (!(*pte & PTE_V))
            continue;

        if (arch_pte_is_leaf(*pte)) {
            int r = mm_fork_clone_leaf(child, parent, pte, entry_base, level, shared);
            if (r < 0)
                return r;
            continue;
        }

        if (level > 0) {
            int r = mm_fork_clone_present_level(child, parent, arch_pte_to_ptr(*pte),
                                                level - 1, entry_base,
                                                start, end, shared);
            if (r < 0)
                return r;
        }
    }
    return 0;
}

int mm_fork_clone_present_range(mm_struct_t *child, mm_struct_t *parent,
                                vaddr_t start, vaddr_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    return mm_fork_clone_present_level(child, parent, parent->pgdir, ARCH_PT_ROOT_LEVEL,
                                       0, start, end, shared);
}

#else /* CONFIG_NOMMU */

/* There is no page table to clone and nothing to copy: a NOMMU address space is
 * flat, so every mapping in it is already shared between parent and child and
 * there is no write protection to drop.  Returning 0 is the honest answer, and
 * it is the one mm_fork() in mm/vm.c understands -- it fails a clone only on a
 * negative return, and those call sites are not guarded, so removing these
 * functions outright would not build.
 *
 * They could not simply have stayed unguarded either.  mm_pt_leaf_table(),
 * mm_pt_peek() and mm_pt_sync_status() are declared in mm/pt.h inside the
 * guard that also tests !CONFIG_NOMMU, so under NOMMU these bodies referenced
 * declarations that do not exist -- an implicit declaration that -Werror turns
 * into a build failure on every NOMMU instance. */
int mm_fork_clone_page(mm_struct_t *child, mm_struct_t *parent, vaddr_t va,
                       int shared)
{ (void)child; (void)parent; (void)va; (void)shared; return 0; }

int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                        vaddr_t start, vaddr_t end, int shared)
{ (void)child; (void)parent; (void)start; (void)end; (void)shared; return 0; }

int mm_fork_clone_leaf(mm_struct_t *child, mm_struct_t *parent,
                       pte_t *src_pte, vaddr_t va, int level, int shared)
{ (void)child; (void)parent; (void)src_pte; (void)va; (void)level; (void)shared; return 0; }

int mm_fork_clone_present_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared)
{ (void)child; (void)parent; (void)table; (void)level; (void)base;
  (void)start; (void)end; (void)shared; return 0; }

int mm_fork_clone_present_range(mm_struct_t *child, mm_struct_t *parent,
                                vaddr_t start, vaddr_t end, int shared)
{ (void)child; (void)parent; (void)start; (void)end; (void)shared; return 0; }

#endif /* CONFIG_NOMMU */
