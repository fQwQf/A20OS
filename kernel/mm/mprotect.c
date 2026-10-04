#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/fault.h"
#include "mm/swap.h"
#include "mm/pt.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/errno.h"

/* mprotect: permission changes on an existing address range. */

int mm_mprotect_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                           int prot) {
    if (!mm || !mm->pgdir) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return 0;

    /* W^X: raising an existing mapping to W|X is bound by the same policy as
     * a new mapping */
    prot = mm_wx_filter_prot(prot, "mprotect");
    if (prot < 0) return prot;

    pte_t ptef = mm_prot_to_pte_flags(prot);
    uint64_t vm_prot = 0;
    if (prot & 1) vm_prot |= VM_READ;
    if (prot & 2) vm_prot |= VM_WRITE;
    if (prot & 4) vm_prot |= VM_EXEC;
    vaddr_t end = addr + len;
    if (end < addr || end > USER_VA_LIMIT) return -ENOMEM;
    mm_seg_index_invalidate(mm);
#ifndef CONFIG_NOMMU
    int touched = 0;
#endif

    vaddr_t covered = addr;
    for (mm_seg_t *v = mm_seg_find(mm, addr); v && covered < end; v = v->next) {
        if (v->start > covered)
            break;
        if (v->end > covered)
            covered = v->end;
    }
    if (covered < end)
        return -ENOMEM;

    /* mseal(2): mprotect over a sealed VMA is refused. */
    for (mm_seg_t *v = mm_seg_find(mm, addr); v && v->start < end; v = v->next) {
        if (v->start >= end || v->end <= addr)
            continue;
        if (v->vm_flags & VM_SEALED)
            return -EPERM;
    }

#ifdef CONFIG_NOMMU
    /* NOMMU has no page tables. We only update the VMA permission bits without splitting. */
    for (mm_seg_t *v = mm_seg_find(mm, addr); v && v->start < end; ) {
        mm_seg_t *next = v->next;
        v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef);
        v->vm_flags  = (v->vm_flags & ~(uint64_t)(VM_READ | VM_WRITE | VM_EXEC)) |
                       vm_prot;
        v = next;
    }
    return 0;
#else
    int     r = mm_split_vma_at(mm, addr);
    if (r < 0) return r;
    r = mm_split_vma_at(mm, end);
    if (r < 0) return r;


    for (mm_seg_t *v = mm_seg_find(mm, addr); v && v->start < end; ) {
        mm_seg_t *next = v->next;
        uint64_t s = v->start < addr ? addr : v->start;
        uint64_t e = v->end > end ? end : v->end;

        /* A split renames both halves, and stops naming what was cut away.
         * vma_split() narrows the head in place and hands back the tail, so
         * the pre-split extent has to be retired BEFORE the call: afterwards
         * the head no longer knows what it used to cover.  Leaving it named
         * makes lookup match on a stale extent, which is how a mprotect split
         * left a neighbouring mapping resolving to the wrong segment --
         * measured as seg_diff on the real-software gate. */
        if (s > v->start) {
            mm_seg_t *head = v;
            mm_mmap_seg_retire(mm, v, v->start, v->end);
            v = vma_split(mm, v, s);
            if (!v) return -ENOMEM;
            next = v->next;
            mm_mmap_seg_annotate(mm, head, head->start, head->end);
            mm_mmap_seg_annotate(mm, v, v->start, v->end);
        }
        if (e < v->end) {
            mm_seg_t *head = v;
            mm_mmap_seg_retire(mm, v, v->start, v->end);
            if (!vma_split(mm, v, e)) return -ENOMEM;
            next = v->next;
            mm_mmap_seg_annotate(mm, head, head->start, head->end);
            mm_mmap_seg_annotate(mm, next, next->start, next->end);
        }

        for (uint64_t va = v->start; va < v->end; ) {
            int level = 0;
            vaddr_t base = 0;
            size_t size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (pte && (*pte & PTE_V)) {
                /* Demote any large leaf, not just one that straddles a VMA
                 * edge.  The loop body below rewrites exactly one leaf entry
                 * and then advances `va = base + size`, i.e. it skips the
                 * rest of the leaf.  That is only sound once `size` is a
                 * single page, so a level>0 leaf must be split first even when
                 * it sits entirely inside this VMA.  Skipping the demotion
                 * left every entry after the first at its old permissions
                 * while v->pte_flags below was rewritten for the whole VMA --
                 * which is how a read-only PTE ended up under a writable VMA
                 * and took a write fault (docs 10.39). */
                if (level > 0) {
                    int dr = mm_demote_huge_page(mm, va);
                    if (dr < 0) return dr;
                    continue;
                }
                uint64_t old_flags = arch_pte_flags(*pte);
                uint64_t flags = mm_pte_flags_apply_prot(*pte, ptef);
                if ((ptef & PTE_W) &&
                    (v->vm_flags & VM_FILE) &&
                    !(v->vm_flags & VM_SHARED)) {
                    pfn_t pfn = phys_to_pfn(arch_pte_addr(*pte));
                    page_cache_page_t *page =
                        mm_file_cache_mapping_get(v, va, pfn);
                    if (page) {
                        /* Keep the canonical cache page read-only.  The first
                         * store will copy it in handle_cow_fault(). */
                        flags &= ~(uint64_t)(PTE_W | PTE_D);
                        flags |= PTE_COW;
                        page_cache_put(page);
                    }
                }
                if ((flags & PTE_X) && !(old_flags & PTE_X)) {
                    paddr_t pa = arch_pte_addr(*pte);
                    pfn_t pfn = phys_to_pfn(pa);
                    if (pfn_valid(pfn))
                        arch_flush_icache_range(pfn_to_virt(pfn), PAGE_SIZE);
                }
                pte_t replacement = arch_pte_leaf(arch_pte_addr(*pte), flags);
                if (replacement != *pte) {
                    *pte = replacement;
                    mm_tlb_note_change(mm, base, size);
                }
                /* The status byte is what a later status-driven fault
                 * installs and what mm_pt_audit_all() compares, so it has to
                 * follow the PTE here as well -- not only on the
                 * never-faulted branch below.  Leaving it stale is what
                 * produced prot_mismatch=5 on a real workload.
                 *
                 * Take the table from mm_pt_leaf_table() rather than deriving
                 * it as `pte - vpn`: that is pointer arithmetic on a pointer
                 * whose provenance is a lookup, and the same trap is
                 * documented on the absent branch below. */
                pte_t *ltab = mm_pt_leaf_table(mm->pgdir, va);
                if (ltab)
                    (void)mm_pt_refresh_leaf_prot(ltab, arch_pt_vpn(va, 0),
                                                  flags);
                va = base + size;
            } else {
                /* Reserved by mmap but never faulted: there is no PTE to carry
                 * the new permissions, and the per-PTE status is what a later
                 * fault will install.  Refresh it here, or mprotect is silently
                 * ignored for this page.
                 *
                 * `pte` may be NULL, not merely non-present: this branch also
                 * catches addresses with no leaf table at all.  The per-PTE
                 * status lives in that table's metadata, so when there is no
                 * table there is no status to refresh.  Computing `pte - idx`
                 * from NULL is pointer arithmetic on a null pointer (UBSAN
                 * flagged it on every such page) and handed
                 * mm_pt_refresh_leaf_prot() a wild pointer, so the refresh
                 * silently did nothing: the status kept its OLD permissions,
                 * v->pte_flags below was updated anyway, and a later
                 * status-driven fault installed the stale permissions --
                 * leaving a PTE that disagreed with its own VMA. */
                /* Ask for the table that owns this leaf slot, NOT for a PTE
                 * inside it.  pt_lookup_leaf() stops at the first non-present
                 * entry, which is very often the level-0 entry of a table
                 * that does exist -- so its NULL said nothing about whether a
                 * per-PTE status was waiting to be installed.  Gating the
                 * refresh on that NULL skipped it for exactly the pages that
                 * were about to fault from the status, leaving a stale
                 * ANON_VIRT prot behind; a later status fault then installed
                 * it verbatim, putting a read-only PTE under a writable VMA
                 * and killing the first store (docs 10.39-10.56).
                 *
                 * If there is no table at all there is nothing to refresh
                 * either: the table a future fault creates has its metadata
                 * memset to zero by mm_pt_node_init(), i.e. all
                 * MM_ST_INVALID, so the status path cannot match
                 * MM_ST_ANON_VIRT and cannot act on a stale prot. */
                pte_t *ltab = mm_pt_leaf_table(mm->pgdir, va);
                if (ltab)
                    (void)mm_pt_refresh_leaf_prot(ltab, arch_pt_vpn(va, 0),
                                                    ptef);
                va += PAGE_SIZE;
            }
        }
        v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef);
        v->vm_flags  = (v->vm_flags & ~(uint64_t)(VM_READ | VM_WRITE | VM_EXEC)) |
                       vm_prot;
        v = vma_try_merge(mm, v);
        touched = 1;
        v = v ? v->next : next;
    }

    (void)touched;
#endif
    return 0;
}

int mm_mprotect(mm_struct_t *mm, vaddr_t addr, size_t len, int prot)
{
    if (!mm) return -EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = mm_mprotect_locked(mm, addr, len, prot);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    return r;
}
