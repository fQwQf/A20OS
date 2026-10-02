#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vmo.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/perf.h"

/*
 * VMA list management: sorted non-overlapping vm_area_t chain plus the backing
 * resource reference helpers used by split/merge/teardown.  Kept separate from
 * the page-table operations in mm/vm.c so the hot map/unmap/protect paths stay
 * readable.
 */

/*
 * Decide whether two adjacent VMAs may coalesce into one.  Merging is only
 * sound when the merged VMA would map exactly the same backing store over the
 * same permissions, so every case below is a correctness precondition rather
 * than a heuristic:
 *
 * - Adjacency: a->end == b->start.  The list is kept sorted, so this also
 *   asserts the caller's ordering.
 * - VM_SYSV_SHM: never merged.  SysV segments are identified by shmid in
 *   ipc/sysv_shm.c, not by file offset, so two adjacent segments are distinct
 *   objects that must stay separable for shmctl(IPC_RMID).
 * - Identical vm_flags and pte_flags: the merged PTE would otherwise have to
 *   pick one protection for the union of two different ones.
 * - VM_FILE: the same open file description AND contiguous file offset.  The
 *   offset check is what keeps a->end mapping a->file_offset + length; without
 *   it a merged VMA would shift b's data.
 * - VM_VMO: the same vmo AND contiguous vmo_offset, for the same reason.  The
 *   second test is redundant with the vm_flags equality above but is kept
 *   explicit because the offset is a separate field.
 * - Anything else (anonymous, stack, guard) is always mergeable.
 *
 * A false negative only costs a VMA, so the checks err toward refusing.
 */
static int vma_can_merge(vm_area_t *a, vm_area_t *b)
{
    if (!a || !b || a->end != b->start)
        return 0;
    if ((a->vm_flags | b->vm_flags) & VM_SYSV_SHM)
        return 0;
    if (a->vm_flags != b->vm_flags || a->pte_flags != b->pte_flags)
        return 0;
    if ((a->vm_flags | b->vm_flags) & VM_FILE) {
        if (a->file != b->file)
            return 0;
        return a->file_offset + (a->end - a->start) == b->file_offset;
    }
    if ((a->vm_flags | b->vm_flags) & VM_VMO) {
        if (!(a->vm_flags & VM_VMO) || !(b->vm_flags & VM_VMO))
            return 0;
        if (a->vmo != b->vmo)
            return 0;
        return a->vmo_offset + (a->end - a->start) == b->vmo_offset;
    }
    return 1;
}

void vma_release_file(vm_area_t *vma)
{
    if (vma && (vma->vm_flags & VM_FILE) && vma->file) {
        if (vma->file_vnode) {
            if (vma->vm_flags & VM_SHARED)
                vnode_shared_map_dec(vma->file_vnode);
            vnode_put(vma->file_vnode);
            vma->file_vnode = NULL;
        }
        vfs_put_file(vma->file);
        vma->file = NULL;
    }
}

void vma_release_ipc(vm_area_t *vma)
{
    if (vma && (vma->vm_flags & VM_SYSV_SHM))
        sysv_shm_unref_attach(vma->sysv_shmid);
}

void vma_release(vm_area_t *vma)
{
    vma_release_file(vma);
    vma_release_ipc(vma);
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo) {
        vmo_release(vma->vmo);
        vma->vmo = NULL;
        vma->vm_flags &= ~(uint64_t)VM_VMO;
    }
}

int vma_ref_file(vm_area_t *vma)
{
    if (!vma || !(vma->vm_flags & VM_FILE) || !vma->file)
        return 0;
    if (vma->file_vnode) {
        vnode_get(vma->file_vnode);
        if (vma->vm_flags & VM_SHARED)
            vnode_shared_map_inc(vma->file_vnode);
    }
    /* The forked/copied VMA owns its own vfile reference. */
    vfile_get(vma->file);
    return 0;
}

int vma_ref_fork(vm_area_t *vma)
{
    int r = vma_ref_file(vma);
    if (r < 0)
        return r;
    if (vma && (vma->vm_flags & VM_SYSV_SHM)) {
        r = sysv_shm_ref_attach(vma->sysv_shmid);
        if (r < 0) {
            vma_release_file(vma);
            return r;
        }
    }
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo)
        vmo_ref(vma->vmo);
    return 0;
}

/*
 * Retain every backing resource a copied VMA must own independently of the
 * original.  Used by the split paths (vma_split, mm_split_vma_at, munmap
 * split) after the tail VMA has been struct-copied from the head.
 */
int vma_ref_aux(vm_area_t *vma)
{
    int r = vma_ref_file(vma);
    if (r < 0)
        return r;
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo)
        vmo_ref(vma->vmo);
    return 0;
}

void mm_vma_index_invalidate(mm_struct_t *mm)
{
    if (!mm)
        return;
    mm->vma_index_state = 0;
    mm->vma_index_count = 0;
}

static void mm_vma_index_rebuild(mm_struct_t *mm, size_t *steps)
{
    size_t count = 0;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (steps)
            (*steps)++;
        if (count == MM_VMA_INDEX_CAPACITY) {
            mm->vma_index_count = 0;
            mm->vma_index_state = 2;
            return;
        }
        mm->vma_index[count++] = v;
    }
    mm->vma_index_count = (uint16_t)count;
    mm->vma_index_state = 1;
}

vm_area_t *mm_find_vma(mm_struct_t *mm, vaddr_t addr) {
    size_t steps = 0;
    a20_perf_count(A20_PERF_VMA_LOOKUPS);

    if (mm->vma_index_state == 0)
        mm_vma_index_rebuild(mm, &steps);

    if (mm->vma_index_state == 1) {
        size_t lo = 0;
        size_t hi = mm->vma_index_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            steps++;
            if (mm->vma_index[mid]->start <= addr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo > 0) {
            vm_area_t *v = mm->vma_index[lo - 1];
            steps++;
            if (addr < v->end) {
                a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
                return v;
            }
        }
        a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
        return NULL;
    }

    /* Very unusual address spaces with more than 256 VMAs retain the proven
     * linked-list behavior instead of allocating while mm->lock is held. */
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        steps++;
        if (steps > 100000u) {
            kerr("[VMAWALK] CYCLE pid? mm=%p addr=0x%lx steps=%u head=%p\n",
                 (void *)mm, (unsigned long)addr, (unsigned)steps,
                 (void *)mm->mmap);
            panic("mm_find_vma: VMA list cycle");
        }
        if (addr < v->end && addr >= v->start) {
            a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
            return v;
        }
        if (v->start > addr)
            break;
    }
    a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
    return NULL;
}

/*
 * Return a temporary reference when a file VMA leaf is backed directly by
 * the canonical page-cache page.  A private file mapping can contain both
 * kinds of leaves: clean read-only cache pages and anonymous pages created by
 * COW.  Comparing the PFN, rather than classifying the whole VMA, keeps
 * fork/mprotect/mremap/teardown correct after only some pages were copied.
 */
page_cache_page_t *mm_file_cache_mapping_get(vm_area_t *vma, vaddr_t va,
                                              pfn_t pfn)
{
    if (!vma || !(vma->vm_flags & VM_FILE) || !vma->file_vnode ||
        va < vma->start || va >= vma->end || !pfn_valid(pfn))
        return NULL;
    uint64_t index = vma->file_offset + (va - vma->start);
    index /= PAGE_SIZE;
    page_cache_page_t *page = page_cache_get(vma->file_vnode, index, 0);
    if (page && page_cache_pfn(page) == pfn)
        return page;
    if (page)
        page_cache_put(page);
    return NULL;
}

vaddr_t mm_find_gap(mm_struct_t *mm, vaddr_t hint, size_t len) {
    vaddr_t prev_end = hint;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= prev_end && v->start - prev_end >= len) return prev_end;
        if (v->end > prev_end) prev_end = v->end;
    }
    return prev_end;
}

int mm_range_overlaps(mm_struct_t *mm, vaddr_t start, vaddr_t len,
                      vm_area_t *ignore) {
    vaddr_t end = start + len;
    if (end < start) return 1;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v == ignore) continue;
        if (v->start < end && v->end > start)
            return 1;
        if (v->start >= end) break;
    }
    return 0;
}

/*
 * MM_AS_VMA_REFCOUNT -- VMA lifetime.
 *
 * vm_area_t carries a reference count so a page fault can read a VMA's fields
 * with mm->lock released; that is what stops one address-space lock from
 * serialising every fault in the process.  Ownership: the address-space list
 * owns the reference created at allocation, unlinking drops it, and the LAST
 * holder -- which may be a fault running with no locks at all -- is what
 * schedules the free.
 *
 * vma_release() can run blocking I/O (vfs_close -> page cache writeback), so
 * the free is never performed inline; the last holder pushes onto the deferred
 * list under vma_ref_lock and an existing flush point drains it.  That lock is
 * deliberately NOT mm->lock: a lock-free fault must be able to defer its free
 * without re-acquiring the lock it just escaped, and holding mm->lock across
 * vma_release() is what the deferred list exists to avoid.
 */
void vma_get(vm_area_t *vma)
{
    if (vma)
        refcount_inc(&vma->refcount);
}

void vma_put(mm_struct_t *mm, vm_area_t *vma)
{
    if (!mm || !vma) return;
    if (!refcount_dec_and_test(&vma->refcount))
        return;

    uint64_t flags = spin_lock_irqsave(&mm->vma_ref_lock);
    vma->deferred_next = mm->deferred_vma;
    mm->deferred_vma = vma;
    spin_unlock_irqrestore(&mm->vma_ref_lock, flags);
}

// Unlink-time release: drops the address-space list's reference.  Callers
// keep their existing shape -- they unlink under mm->lock and drop it before
// flushing, so the observable ordering is unchanged.
void mm_vma_defer(mm_struct_t *mm, vm_area_t *vma)
{
    vma_put(mm, vma);
}

void mm_vma_flush_deferred(mm_struct_t *mm)
{
    if (!mm) return;

    /*
     * Detach the whole list under vma_ref_lock so two threads sharing an mm
     * cannot both observe and free the same chain.  The detached list is
     * private to this flusher; potentially sleeping vma_release() work stays
     * outside the spinlock.
     */
    uint64_t flags = spin_lock_irqsave(&mm->vma_ref_lock);
    vm_area_t *v = mm->deferred_vma;
    mm->deferred_vma = NULL;
    spin_unlock_irqrestore(&mm->vma_ref_lock, flags);

    while (v) {
        vm_area_t *next = v->deferred_next;
        vma_release(v);
        kfree(v);
        v = next;
    }
}

void mm_insert_vma(mm_struct_t *mm, vm_area_t *newv) {
    mm_vma_index_invalidate(mm);
    vm_area_t **pp = &mm->mmap;
    vm_area_t *prev = NULL;
    while (*pp && (*pp)->start < newv->start) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    newv->next = *pp;
    newv->prev = prev;
    if (*pp) (*pp)->prev = newv;
    *pp = newv;

    if (vma_can_merge(newv, newv->next)) {
        vm_area_t *nxt = newv->next;
        newv->end = nxt->end;
        newv->next = nxt->next;
        if (nxt->next) nxt->next->prev = newv;
        mm_vma_defer(mm, nxt);
    }
    if (vma_can_merge(newv->prev, newv)) {
        vm_area_t *prv = newv->prev;
        prv->end = newv->end;
        prv->next = newv->next;
        if (newv->next) newv->next->prev = prv;
        mm_vma_defer(mm, newv);
    }
}

int mm_split_vma_at(mm_struct_t *mm, vaddr_t addr) {
    vm_area_t *v = mm_find_vma(mm, addr);
    if (!v || addr <= v->start || addr >= v->end)
        return 0;

    vm_area_t *tail = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!tail)
        return -ENOMEM;

    *tail = *v;
    refcount_set(&tail->refcount, 1);
    tail->start = addr;
    tail->file_offset += addr - v->start;
    int fr = vma_ref_aux(tail);
    if (fr < 0) {
        kfree(tail);
        return fr;
    }
    mm_vma_index_invalidate(mm);
    tail->prev = v;
    tail->next = v->next;
    if (tail->next)
        tail->next->prev = tail;

    v->end = addr;
    v->next = tail;
    return 0;
}

vm_area_t *vma_split(vm_area_t *vma, vaddr_t split) {
    if (!vma) return NULL;
    if (split <= vma->start || split >= vma->end) return vma;

    vm_area_t *tail = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!tail) return NULL;

    *tail = *vma;
    refcount_set(&tail->refcount, 1);
    tail->start = split;
    tail->file_offset += split - vma->start;
    if (vma_ref_aux(tail) < 0) {
        kfree(tail);
        return NULL;
    }
    tail->prev = vma;
    tail->next = vma->next;
    if (tail->next) tail->next->prev = tail;

    vma->end = split;
    vma->next = tail;
    return tail;
}

vm_area_t *vma_try_merge(mm_struct_t *mm, vm_area_t *vma) {
    if (!vma) return NULL;

    mm_vma_index_invalidate(mm);

    if (vma_can_merge(vma->prev, vma)) {
        vm_area_t *prev = vma->prev;
        prev->end = vma->end;
        prev->next = vma->next;
        if (vma->next) vma->next->prev = prev;
        mm_vma_defer(mm, vma);
        vma = prev;
    }

    if (vma_can_merge(vma, vma->next)) {
        vm_area_t *next = vma->next;
        vma->end = next->end;
        vma->next = next->next;
        if (next->next) next->next->prev = vma;
        mm_vma_defer(mm, next);
    }
    return vma;
}

void free_vma_pages(mm_struct_t *mm, vm_area_t *vma)
{
#ifdef CONFIG_NOMMU
    (void)mm;
    if (vma->nommu_alloc) {
        kfree(vma->nommu_alloc);
        vma->nommu_alloc = NULL;
    }
    /* In NOMMU, we now track kmalloc allocations in mm->nommu_allocs.
     * We don't free vma->start directly here to avoid double-frees and freeing
     * invalid pointers if the VMA was split or modified by mprotect/munmap. */
#else
    if (!mm->pgdir) return;
    int shared_file = (vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED);
    for (uint64_t va = vma->start; va < vma->end; ) {
        /* Check whether the current mapping is a huge page that straddles a
         * VMA boundary; if so, demote it to base pages and release them
         * one at a time. */
        int level = 0;
        vaddr_t base = 0;
        size_t size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (pte && (*pte & PTE_V) && level > 0 &&
            (base < vma->start || base + size > vma->end)) {
            mm_demote_huge_page(mm, va);
            continue;
        }

        if (pte && (*pte & PTE_V) && shared_file && vma->file_vnode &&
            (*pte & PTE_D)) {
            uint64_t idx = vma->file_offset + (va - vma->start);
            idx /= PAGE_SIZE;
            page_cache_page_t *pcp = page_cache_get(vma->file_vnode, idx, 0);
            if (pcp) {
                page_cache_mark_dirty(pcp);
                page_cache_put(pcp);
            }
        }

        page_cache_page_t *mapped_page = NULL;
        if (pte && (*pte & PTE_V)) {
            pfn_t mapped_pfn = phys_to_pfn(arch_pte_addr(*pte));
            mapped_page = mm_file_cache_mapping_get(vma, va, mapped_pfn);
        }

        paddr_t pa = 0;
        base = 0;
        size = 0;
        if (pt_unmap_leaf(mm, va, &pa, &base, &size, NULL) == 0) {
            if (pa) {
                pfn_t pfn = phys_to_pfn(pa);
                if (mapped_page) {
                    /* Temporary lookup plus the PTE's retained cache pin. */
                    page_cache_put(mapped_page);
                    page_cache_put(mapped_page);
                } else if (!(vma->vm_flags & (VM_PFNMAP | VM_VMO))) {
                    /* VMO frames are owned by the VMO (see vmo_get_page);
                     * unmapping a PTE never releases them — same rule as
                     * mm_munmap_locked()/madvise.  Putting them here freed
                     * live VMO pages while other mappers (and vmo->pages[])
                     * still referenced them. */
                    frame_put(pfn);
                }
            }
            va = base + size;
        } else {
            if (mapped_page)
                page_cache_put(mapped_page);
            va += PAGE_SIZE;
        }
    }
#endif
}
