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

/* Unmap paths: munmap and brk shrink. */

/* An anonymous range that mm_pt_provision_anon() reserved but that was never
 * faulted has a status mark and no PTE, so the loops below skip it and
 * pt_unmap_leaf() never sees it.  Tearing the mapping down has to retire that
 * mark: mm_fault_from_status() serves any MM_ST_ANON_VIRT it finds without
 * consulting a VMA, so a mark left behind lets a later access into the hole
 * map a page instead of failing.
 *
 * The table comes from mm_pt_leaf_table(), not from the `pte` the caller got
 * back.  On this branch `pte` is NULL whenever the address has no leaf table
 * at all, and it is also NULL when pt_lookup_leaf() merely stopped at a
 * non-present entry inside a table that DOES exist -- so deriving the table as
 * `pte - vpn` is pointer arithmetic on NULL and silently skipped the very
 * pages that were about to fault from the status.  mm_pt_leaf_table() walks
 * from the root without allocating, so the teardown path cannot conjure the
 * intermediate levels it is in the middle of destroying. */
#ifndef CONFIG_NOMMU
static void mm_munmap_retire_reservation(mm_struct_t *mm, vaddr_t va)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    pte_t *table = mm_pt_leaf_table(mm->pgdir, va);
    if (!table || mm_pt_meta_level(mm_pt_meta(table)) != 0)
        return;
    int idx = arch_pt_vpn(va, 0);
    mm_pt_node_lock(table);
    if (MM_ST_GET_CLASS(mm_pt_peek(table, 0, idx)) == MM_ST_ANON_VIRT)
        mm_pt_note_absent(table, 0, idx);
    mm_pt_node_unlock(table);
#else
    /* arm32's short-descriptor backend keeps no status marks, so a
     * never-faulted reservation cannot exist to be retired. */
    (void)mm;
    (void)va;
#endif /* ARCH_HAS_PGTABLE_OPS */
}
#endif /* CONFIG_NOMMU */
/*
 * The unmap transaction lives in kernel/mm/munmap_txn.rs.  What is left here
 * is what it cannot name: the mm_seg_t layout and the list surgery, the
 * page-table walk and leaf teardown, the architecture predicates (presence,
 * swap, leaf, physical address, dirty bit), and the TLB hold/release
 * primitives.  Every bridge is one field read, one field write, or one call to
 * an existing helper -- none of them decides anything.
 */
extern int a20_munmap_rust(mm_struct_t *mm, vaddr_t addr, size_t len);

struct a20_munmap_vma_info {
    uintptr_t start;
    uintptr_t end;
    uint64_t  vm_flags;
    uint64_t  backing_offset;
    void     *file_vnode;
};

struct a20_munmap_leaf {
    uintptr_t aux;    /* probe: raw PTE; unmap: the reported physical address */
    uintptr_t base;
    uintptr_t size;
    uint32_t  pfn;
    int32_t   level;
    int32_t   present;
    int32_t   swap;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_munmap_leaf) == 40, "Rust/C unmap leaf ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct a20_munmap_leaf) == 28, "Rust/C unmap leaf ABI");
#endif

struct a20_munmap_vma_bounds {
    uintptr_t start;
    uintptr_t end;
    uint64_t  vm_flags;
};

struct a20_munmap_entry {
    void     *vma;
    uintptr_t start;
    uintptr_t end;
    uint64_t  vm_flags;
    uint64_t  backing_offset;
    void     *file_vnode;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_munmap_entry) == 48, "Rust/C unmap entry ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct a20_munmap_entry) == 24, "Rust/C unmap entry ABI");
#endif


int a20_munmap_has_pgdir(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm && mm->pgdir;
}

uintptr_t a20_munmap_user_va_limit(void)
{
    return USER_VA_LIMIT;
}

void *a20_munmap_vma_first(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm ? mm->mmap : NULL;
}

void *a20_munmap_vma_next(void *vma_ptr)
{
    mm_seg_t *v = vma_ptr;
    return v ? v->next : NULL;
}

void a20_munmap_vma_info(void *vma_ptr, struct a20_munmap_vma_info *out)
{
    mm_seg_t *v = vma_ptr;
    if (!v || !out)
        return;
    out->start = v->start;
    out->end = v->end;
    out->vm_flags = v->vm_flags;
    out->backing_offset = v->backing_offset;
    out->file_vnode = v->file_vnode;
}

int a20_munmap_is_nommu(void)
{
#ifdef CONFIG_NOMMU
    return 1;
#else
    return 0;
#endif
}

/* NOMMU has no leaves: a mapping is one flat allocation this record owns, so
 * releasing a range means freeing (or untracking) it, and only a whole-record
 * clip does.  This is the branch the C reference kept on the other side of its
 * `#ifdef CONFIG_NOMMU`, restored as a bridge. */
void a20_munmap_release_flat(void *mm_ptr, void *vma_ptr, uintptr_t clip_start,
                             uintptr_t clip_end)
{
#ifdef CONFIG_NOMMU
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *v = vma_ptr;
    if (!v || v->start != clip_start || v->end != clip_end)
        return;
    if (v->nommu_alloc) {
        kfree(v->nommu_alloc);
        v->nommu_alloc = NULL;
    } else {
        mm_untrack_nommu_alloc(mm, (void *)v->start);
    }
#else
    (void)mm_ptr;
    (void)vma_ptr;
    (void)clip_start;
    (void)clip_end;
#endif
}

void a20_munmap_vma_bounds(void *vma_ptr, struct a20_munmap_vma_bounds *out)
{
    mm_seg_t *v = vma_ptr;
    if (!v || !out)
        return;
    out->start = v->start;
    out->end = v->end;
    out->vm_flags = v->vm_flags;
}

/* Collect the records the range covers in one walk.  The transaction only
 * operates on covered records, so walking the (often much longer) prefix in C
 * keeps a call off every visited record; the Rust side then decides what to do
 * with the short list.  -ENOSPC means the caller must walk the list itself. */
int a20_munmap_collect(void *mm_ptr, uintptr_t addr, uintptr_t end,
                       struct a20_munmap_entry *out, uintptr_t cap)
{
    mm_struct_t *mm = mm_ptr;
    uintptr_t n = 0;
    if (!mm || !out)
        return -EINVAL;
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= end)
            break;
        if (v->end <= addr)
            continue;
        if (n == cap)
            return -ENOSPC;
        struct a20_munmap_entry *e = &out[n++];
        e->vma = v;
        e->start = v->start;
        e->end = v->end;
        e->vm_flags = v->vm_flags;
        e->backing_offset = v->backing_offset;
        e->file_vnode = v->file_vnode;
    }
    return (int)n;
}

void a20_munmap_account(void *mm_ptr, uintptr_t freed_pages,
                        uintptr_t locked_bytes)
{
    mm_struct_t *mm = mm_ptr;
    if (!mm)
        return;
    mm->total_vm = (mm->total_vm > freed_pages) ? mm->total_vm - freed_pages : 0;
    if (locked_bytes)
        mm->locked_vm = (mm->locked_vm >= locked_bytes)
                            ? mm->locked_vm - locked_bytes
                            : 0;
}

/* Whole record: the range loses its backing object, so its label goes with it.
 * The per-node collapse in pt_unmap_leaf only runs for tables that held no
 * leaves, which is exactly not the case for a mapping that was never faulted.
 * Only this record's own name is dropped: the node entries here are far
 * coarser than the mapping and may also be naming a live neighbour. */
void a20_munmap_drop_whole(void *mm_ptr, void *vma_ptr, uintptr_t clip_start,
                           uintptr_t clip_end)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *v = vma_ptr;
    if (!mm || !v)
        return;
    mm_mmap_seg_retire(mm, v, clip_start, clip_end);
    if (v->prev) v->prev->next = v->next;
    else mm->mmap = v->next;
    if (v->next) v->next->prev = v->prev;
    mm_vma_defer(mm, v);
}

/* Head cut: the survivor's offset moves, so the extent it used to name is
 * retired before the bounds change and the new one is named after. */
void a20_munmap_cut_head(void *mm_ptr, void *vma_ptr, uintptr_t clip_end)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *v = vma_ptr;
    if (!mm || !v)
        return;
    v->backing_offset += clip_end - v->start;
    mm_mmap_seg_retire(mm, v, v->start, clip_end);
    v->start = clip_end;
    mm_mmap_seg_annotate(mm, v, v->start, v->end);
}

/* Tail cut: the offset still lines up -- only the extent shrank.  The extent is
 * exactly what the segment lookup matches on, so it has to be re-named even
 * though the record is the same one. */
void a20_munmap_cut_tail(void *mm_ptr, void *vma_ptr, uintptr_t clip_start)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *v = vma_ptr;
    if (!mm || !v)
        return;
    mm_mmap_seg_retire(mm, v, clip_start, v->end);
    v->end = clip_start;
    mm_mmap_seg_annotate(mm, v, v->start, v->end);
}

/* Middle cut: two records now describe one, and both halves need naming
 * afresh -- the head's old name described [old_start, old_end), so it no
 * longer contains [old_start, clip_start).  The head is named FIRST, while the
 * tail is not yet linked, so the two walks cannot both claim the node entry
 * that straddles the boundary.
 *
 * This is the only fallible step after the leaves are released.  A failure
 * returns with the freed range still described by one record and the caller
 * told -ENOMEM, never with two records that overlap. */
int a20_munmap_cut_middle(void *mm_ptr, void *vma_ptr, uintptr_t clip_start,
                          uintptr_t clip_end)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *v = vma_ptr;
    if (!mm || !v)
        return -EINVAL;

    mm_seg_t *tail = mm_seg_new();
    if (!tail)
        return -ENOMEM;
    *tail = *v;
    mm_mmap_seg_retire(mm, v, v->start, v->end);
    refcount_set(&tail->refcount, 1);   /* the copy brought its count along */
    tail->start = clip_end;
    tail->end = v->end;
    tail->backing_offset += clip_end - v->start;
    int fr = vma_ref_aux(tail);
    if (fr < 0) {
        kfree(tail);
        return fr;
    }
    tail->prev = v;
    tail->next = v->next;
    if (v->next) v->next->prev = tail;
    v->next = tail;
    v->end = clip_start;
    mm_mmap_seg_annotate(mm, v, v->start, v->end);
    mm_mmap_seg_annotate(mm, tail, tail->start, tail->end);
    return 0;
}

/* Decode one leaf: the walk, the architecture's presence/swap predicates, and
 * the physical address of a present leaf.  `aux` carries the raw PTE so the
 * harvest bridge can read the dirty bit without this file seeing it. */
void a20_munmap_leaf_probe(void *mm_ptr, uintptr_t va,
                           struct a20_munmap_leaf *out)
{
    mm_struct_t *mm = mm_ptr;
    if (!out)
        return;
    out->aux = 0;
    out->base = 0;
    out->size = 0;
    out->pfn = PFN_NONE;
    out->level = 0;
    out->present = 0;
    out->swap = 0;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &out->level, &out->base,
                                &out->size);
    if (!pte)
        return;
    out->aux = (uintptr_t)*pte;
    out->present = (*pte & PTE_V) != 0;
#ifdef CONFIG_SWAP
    out->swap = pte_is_swap(*pte);
#endif
    if (out->present)
        out->pfn = phys_to_pfn(arch_pte_addr(*pte));
}

void a20_munmap_retire_reservation(void *mm_ptr, uintptr_t va)
{
#ifdef CONFIG_NOMMU
    /* NOMMU keeps no per-PTE status sidecar, so there is no mark to retire. */
    (void)mm_ptr;
    (void)va;
#else
    mm_munmap_retire_reservation(mm_ptr, va);
#endif
}

int a20_munmap_demote(void *mm_ptr, uintptr_t va)
{
    return mm_demote_huge_page(mm_ptr, va);
}

/* Preserve a shared-file mapping's dirty state before the leaf that carries
 * the only evidence of it is released.  Two architecture rules: where a
 * software-visible dirty bit exists, the leaf's own PTE_D is the evidence;
 * where it does not (ARM32), a writable shared mapping is harvested
 * conservatively across the whole leaf. */
void a20_munmap_leaf_harvest(void *mm_ptr, void *vma_ptr, uintptr_t va,
                             uintptr_t clip_start, uintptr_t clip_end,
                             const struct a20_munmap_leaf *leaf, int shared_file)
{
    (void)mm_ptr;
    /* Both clip bounds are read only by the ARM32 conservative rule below. */
    (void)clip_start;
    (void)clip_end;
    mm_seg_t *vma = vma_ptr;
    if (!shared_file || !vma || !vma->file_vnode || !leaf)
        return;

    if (leaf->aux & PTE_D) {
        uint64_t idx = (vma->backing_offset + (va - vma->start)) / PAGE_SIZE;
        page_cache_page_t *pcp = page_cache_get(vma->file_vnode, idx, 0);
        if (pcp) {
            page_cache_mark_dirty(pcp);
            page_cache_put(pcp);
        }
    }
#if PTE_D == 0
    if ((vma->vm_flags & VM_WRITE) && leaf->present &&
        arch_pte_is_leaf((pte_t)leaf->aux)) {
        vaddr_t dirty_start = leaf->base > clip_start ? leaf->base : clip_start;
        uint64_t leaf_end = (uint64_t)leaf->base + leaf->size;
        vaddr_t dirty_end = leaf_end < clip_end ? (vaddr_t)leaf_end : clip_end;
        for (vaddr_t dirty_va = dirty_start; dirty_va < dirty_end;
             dirty_va += PAGE_SIZE) {
            uint64_t idx = vma->backing_offset + (dirty_va - vma->start);
            page_cache_page_t *pcp =
                page_cache_get(vma->file_vnode, idx / PAGE_SIZE, 0);
            if (pcp) {
                page_cache_mark_dirty(pcp);
                page_cache_put(pcp);
            }
        }
    }
#endif
}

/* Pin the leaf's owner before the leaf is unmapped.  A MAP_PRIVATE file leaf
 * may still BE the canonical page-cache frame, whose reference belongs to the
 * cache; classifying by the leaf's PFN rather than by a VMA-wide flag is what
 * keeps a partly-copied private mapping correct. */
int a20_munmap_leaf_hold(void *mm_ptr, void *vma_ptr, uintptr_t va, uint32_t pfn,
                         int no_frame_ref, void **out_pcp)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *vma = vma_ptr;
    page_cache_page_t *pcp = NULL;
    if (out_pcp)
        *out_pcp = NULL;
    if (vma)
        pcp = mm_file_cache_mapping_get(vma, va, pfn);
    if (pcp) {
        if (mm_tlb_hold_page(mm, pcp) < 0) {
            page_cache_put(pcp);
            return -ENOMEM;
        }
    } else if (!no_frame_ref) {
        if (!pfn_valid(pfn) || mm_tlb_hold_frame(mm, pfn) < 0)
            return -ENOMEM;
    }
    if (out_pcp)
        *out_pcp = pcp;
    return 0;
}

int a20_munmap_leaf_unmap(void *mm_ptr, uintptr_t va,
                          struct a20_munmap_leaf *out)
{
    paddr_t pa = 0;
    vaddr_t base = 0;
    size_t size = 0;
    int r = pt_unmap_leaf(mm_ptr, va, &pa, &base, &size, NULL);
    if (out) {
        out->aux = pa;
        out->base = base;
        out->size = size;
    }
    return r;
}

/* Release exactly one reference to exactly one owner, then account.  The
 * page-cache branch drops the temporary lookup and the mapping's own pin; the
 * frame branch is skipped entirely for a leaf this mapping never took a frame
 * reference to (VMO-owned, PFNMAP, the vDSO). */
void a20_munmap_leaf_release(void *mm_ptr, int no_frame_ref, uintptr_t pa,
                             uintptr_t base, uintptr_t size, void *pcp_ptr)
{
    mm_struct_t *mm = mm_ptr;
    page_cache_page_t *pcp = pcp_ptr;
    mm_tlb_note_change(mm, base, size);
    if (!pa)
        return;
    if (pcp) {
        page_cache_put(pcp);
        page_cache_put(pcp);
    } else if (!no_frame_ref) {
        frame_put(phys_to_pfn(pa));
    }
    mm_rss_sub_clamped(mm, size / PAGE_SIZE);
}

void a20_munmap_page_put(void *pcp)
{
    if (pcp)
        page_cache_put(pcp);
}


int mm_munmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len) {
    return a20_munmap_rust(mm, addr, len);
}

vaddr_t mm_brk_locked(mm_struct_t *mm, vaddr_t newbrk) {
    if (!mm || !mm->pgdir) return 0;
    if (newbrk == 0) return mm->brk;
    if (newbrk < mm->start_brk || newbrk > USER_VA_LIMIT)
        return mm->brk;

#ifdef CONFIG_NOMMU
    /* Without page tables, brk cannot grow a contiguous virtual heap. Returning
     * the unchanged break makes libc fall back to mmap-backed allocations. */
    return mm->brk;
#else
    vaddr_t old_brk_page = ROUND_UP(mm->brk, PAGE_SIZE);
    vaddr_t new_brk_page = ROUND_UP(newbrk, PAGE_SIZE);
    if (new_brk_page < newbrk)
        return mm->brk;

    if (newbrk > mm->brk) {
        vaddr_t old_brk = ROUND_UP(mm->brk, PAGE_SIZE);
        vaddr_t new_brk = ROUND_UP(newbrk, PAGE_SIZE);
        if (new_brk > old_brk) {
            if (mm_range_overlaps(mm, old_brk, new_brk - old_brk, NULL))
                return mm->brk;
        }
    }

    if (newbrk < mm->brk) {
        /* mseal(2): refuse to shrink the heap into a sealed VMA. */
        for (mm_seg_t *v = mm->mmap; v; v = v->next) {
            if (v->start >= old_brk_page)
                break;
            if (v->end <= new_brk_page)
                continue;
            if (v->vm_flags & VM_SEALED)
                return mm->brk;
        }
        for (uint64_t va = new_brk_page; va < old_brk_page; ) {
            int level = 0;
            vaddr_t base = 0;
            size_t size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (!pte ||
                (!(*pte & PTE_V)
#ifdef CONFIG_SWAP
                 && !pte_is_swap(*pte)
#endif
                )) {
                mm_munmap_retire_reservation(mm, va);
                va += PAGE_SIZE;
                continue;
            }
            if (level > 0 && (base < new_brk_page || base + size > old_brk_page)) {
                if (mm_demote_huge_page(mm, va) < 0)
                    break;
                continue;
            }
            if ((*pte & PTE_V) &&
                mm_tlb_hold_frame(mm,
                                  phys_to_pfn(arch_pte_addr(*pte))) < 0)
                return mm->brk;
            paddr_t pa = 0;
            if (pt_unmap_leaf(mm, va, &pa, &base, &size, NULL) == 0) {
                mm_tlb_note_change(mm, base, size);
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = size / PAGE_SIZE;
                    mm_rss_sub_clamped(mm, pages);
                }
                va = base + size;
            } else {
                va += PAGE_SIZE;
            }
        }
    }
    /*
     * brk pages are installed lazily by handle_demand_fault().  They still
     * need a VMA: without it, a first write to the grown heap is rejected as
     * an unmapped address before the brk fault path can allocate a frame.
     */
    if (newbrk > mm->brk) {
        vaddr_t map_start = ROUND_UP(mm->brk, PAGE_SIZE);
        vaddr_t map_end = ROUND_UP(newbrk, PAGE_SIZE);
        if (map_end > map_start) {
            mm_seg_t *vma = mm_seg_new();
            if (!vma)
                return mm->brk;
            vma->start = map_start;
            vma->end = map_end;
            vma->vm_flags = VM_ANON | VM_READ | VM_WRITE;
            vma->pte_flags = mm_user_brk_pte_flags();
            mm_insert_vma(mm, vma);
            mm->total_vm += (map_end - map_start) / PAGE_SIZE;
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
            (void)mm_pt_provision_anon(mm, map_start, map_end,
                                        mm_user_brk_pte_flags());
#endif
        }
    }
    mm->brk = newbrk;
    return mm->brk;
#endif
}

int mm_munmap(mm_struct_t *mm, vaddr_t addr, size_t len) {
    if (!mm) return -EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = mm_munmap_locked(mm, addr, len);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    return r;
}

vaddr_t mm_brk(mm_struct_t *mm, vaddr_t newbrk)
{
    if (!mm) return 0;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_brk_locked(mm, newbrk);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    return r;
}
