#include "core/errno.h"
#include "core/lock.h"
#include "core/mman.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/vmo.h"
#include "mm/oom.h"
#include "proc/proc.h"
#include "fs/page_cache.h"

/*
 * VMO-backed region export and madvise/mlock helpers — ABI-agnostic MM
 * operations over the caller's own address space.
 *
 * The advice policy and the two mapping transactions behind it live in
 * kernel/mm/madvise_policy.rs: which advice acts and which is accepted and
 * ignored, the range validation and coverage contract, the discard
 * transaction with its exactly-once release, the fork-policy transaction, and
 * the VMO export qualification.  What is left here is what Rust cannot name:
 * the mm_seg_t fields and the list surgery, the page-table walk and leaf
 * teardown, the TLB hold/release primitives, the page-cache lookup and the
 * task's mm.  None of the bridges below decides anything.
 */

struct a20_madvise_vma {
    void    *vma;
    uintptr_t start;
    uintptr_t end;
    uint64_t  vm_flags;
};

struct a20_madvise_leaf {
    int32_t   present;
    int32_t   level;
    uintptr_t base;
    uintptr_t size;
    uint32_t  pfn;
    uint32_t  pad;
};

struct a20_madvise_unmapped {
    int32_t   ok;
    uintptr_t base;
    uintptr_t size;
    uintptr_t pa;
};

/* Which owner the released leaf's reference belongs to; chosen in Rust. */
#define A20_MADVISE_REL_NONE  0
#define A20_MADVISE_REL_FRAME 1
#define A20_MADVISE_REL_CACHE 2

extern int a20_madvise_dontneed(mm_struct_t *mm, vaddr_t addr, size_t len);
extern int a20_madvise_fork_flags(mm_struct_t *mm, vaddr_t start, size_t len,
                                  uint64_t flags, int on);
extern int a20_madvise_dispatch(mm_struct_t *mm, vaddr_t addr, size_t len,
                                int advice);
extern int a20_madvise_covered_only(mm_struct_t *mm, vaddr_t addr, size_t len);
extern int a20_madvise_set_lock(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                                int on);
extern struct vmo *a20_madvise_vmo_region(mm_struct_t *mm, vaddr_t addr,
                                          size_t len, uint32_t *prot_out);

uintptr_t a20_madvise_user_va_limit(void)
{
    return USER_VA_LIMIT;
}

int a20_madvise_is_nommu(void)
{
#ifdef CONFIG_NOMMU
    return 1;
#else
    return 0;
#endif
}

uint64_t a20_madvise_lock(void *mm_ptr)
{
    mm_struct_t *mm = mm_ptr;
    return spin_lock_irqsave(&mm->lock);
}

void a20_madvise_unlock(void *mm_ptr, uint64_t flags)
{
    mm_struct_t *mm = mm_ptr;
    spin_unlock_irqrestore(&mm->lock, flags);
}

/* The records overlapping [addr, end), in address order.  Batched so the
 * transaction walks the list once instead of paying a bridge call per record;
 * -ENOSPC sends the caller to a20_madvise_vma_at(). */
int a20_madvise_collect(void *mm_ptr, uintptr_t addr, uintptr_t end,
                        struct a20_madvise_vma *out, uintptr_t cap)
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
        struct a20_madvise_vma *e = &out[n++];
        e->vma = v;
        e->start = v->start;
        e->end = v->end;
        e->vm_flags = v->vm_flags;
    }
    return (int)n;
}

int a20_madvise_vma_at(void *mm_ptr, uintptr_t va, struct a20_madvise_vma *out)
{
    mm_struct_t *mm = mm_ptr;
    if (!mm || !out)
        return -EINVAL;
    mm_seg_t *v = mm_seg_find(mm, va);
    if (!v || va >= v->end)
        return -ENOMEM;
    out->vma = v;
    out->start = v->start;
    out->end = v->end;
    out->vm_flags = v->vm_flags;
    return 0;
}

void a20_madvise_vma_set_flags(void *vma_ptr, uint64_t flags, int on)
{
    mm_seg_t *v = vma_ptr;
    if (!v)
        return;
    if (on)
        v->vm_flags |= flags;
    else
        v->vm_flags &= ~flags;
}

uint64_t a20_madvise_vma_flags(void *vma_ptr)
{
    mm_seg_t *v = vma_ptr;
    return v ? v->vm_flags : 0;
}

uintptr_t a20_madvise_vma_pte_flags(void *vma_ptr)
{
    mm_seg_t *v = vma_ptr;
    return v ? (uintptr_t)v->pte_flags : 0;
}

/* The record that fully covers [addr, end), or NULL.  The qualification on
 * VM_VMO and on the vmo pointer itself is the caller's decision. */
void *a20_madvise_vmo_covering(void *mm_ptr, uintptr_t addr, uintptr_t end)
{
    mm_struct_t *mm = mm_ptr;
    if (!mm)
        return NULL;
    mm_seg_t *v = mm_seg_find(mm, addr);
    if (!v || (uintptr_t)v->start > addr || (uintptr_t)v->end < end)
        return NULL;
    return v;
}

/* Take the record's own reference to its VMO and hand it back; NULL when the
 * record has none.  Caller holds mm->lock, so the record cannot go away
 * between the coverage check and this reference. */
struct vmo *a20_madvise_vmo_ref(void *vma_ptr)
{
    mm_seg_t *v = vma_ptr;
    if (!v || !v->vmo)
        return NULL;
    vmo_ref(v->vmo);
    return v->vmo;
}

void *a20_madvise_current_mm(void)
{
    task_t *t = proc_current();
    return t ? t->mm : NULL;
}

/* MADV_PAGEOUT hands the range to the OOM/swap reclaim policy, which needs the
 * task that owns the address space. */
int a20_madvise_pageout_range(void *mm_ptr, uintptr_t addr, uintptr_t end)
{
    (void)mm_ptr;
    return oom_swap_reclaim_range(proc_current(), addr, end - addr);
}

#ifndef CONFIG_NOMMU
int a20_madvise_split(void *mm_ptr, uintptr_t va)
{
    return mm_split_vma_at(mm_ptr, va);
}

void a20_madvise_leaf_probe(void *mm_ptr, uintptr_t va,
                            struct a20_madvise_leaf *out)
{
    mm_struct_t *mm = mm_ptr;
    if (!out)
        return;
    out->present = 0;
    out->level = 0;
    out->base = 0;
    out->size = 0;
    out->pfn = PFN_NONE;
    out->pad = 0;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &out->level, &out->base,
                                &out->size);
    if (!pte)
        return;
    out->present = (*pte & PTE_V) != 0;
    if (out->present)
        out->pfn = phys_to_pfn(arch_pte_addr(*pte));
}

/* Hold the reference the leaf owns, and report the page-cache page (with its
 * lookup reference) when the leaf is a cache page.  On failure the lookup
 * reference has already been given back. */
int a20_madvise_leaf_hold(void *mm_ptr, void *vma_ptr, uintptr_t va,
                          uint32_t pfn, int no_frame_ref, void **out_pcp)
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

int a20_madvise_demote(void *mm_ptr, uintptr_t va)
{
    return mm_demote_huge_page(mm_ptr, va);
}

void a20_madvise_leaf_unmap(void *mm_ptr, uintptr_t va,
                            struct a20_madvise_unmapped *out)
{
    paddr_t pa = 0;
    vaddr_t base = 0;
    size_t size = 0;
    int r = pt_unmap_leaf(mm_ptr, va, &pa, &base, &size, NULL);
    if (out) {
        out->ok = r;
        out->base = base;
        out->size = size;
        out->pa = pa;
    }
}

void a20_madvise_note_change(void *mm_ptr, uintptr_t base, uintptr_t size)
{
    mm_tlb_note_change(mm_ptr, base, size);
}

void a20_madvise_drop_cache_ref(void *pcp_ptr)
{
    if (pcp_ptr)
        page_cache_put(pcp_ptr);
}

/* The exactly-once release, dispatched on the owner Rust chose from the leaf:
 * a cache page is put twice (the mapping's cache pin plus the temporary
 * lookup), a plain anonymous frame once, and a leaf with no per-mapping frame
 * reference is not put at all.  The TLB queue's own reference is released when
 * the flush completes. */
void a20_madvise_release(void *mm_ptr, int kind, void *pcp_ptr, uintptr_t pa,
                         uintptr_t pages)
{
    mm_struct_t *mm = mm_ptr;
    if (kind == A20_MADVISE_REL_NONE) {
        mm_rss_sub_clamped(mm, pages);
        return;
    }
    if (!pa)
        return;
    if (kind == A20_MADVISE_REL_CACHE && pcp_ptr) {
        page_cache_put(pcp_ptr);
        page_cache_put(pcp_ptr);
    } else if (kind == A20_MADVISE_REL_FRAME) {
        frame_put(phys_to_pfn(pa));
    }
    mm_rss_sub_clamped(mm, pages);
}
#else /* CONFIG_NOMMU */
/*
 * NOMMU's pt_lookup_leaf() returns a sentinel pointer rather than a real entry
 * and its pt_unmap_leaf() answers 1, so the discard transaction must not walk
 * anything there -- a20_madvise_is_nommu() returns before it does.  These
 * exist so the Rust object links on every profile, and their bodies are never
 * reached.
 */
int a20_madvise_split(void *mm_ptr, uintptr_t va)
{
    (void)mm_ptr;
    (void)va;
    return 0;
}

void a20_madvise_leaf_probe(void *mm_ptr, uintptr_t va,
                            struct a20_madvise_leaf *out)
{
    (void)mm_ptr;
    (void)va;
    if (out) {
        out->present = 0;
        out->level = 0;
        out->base = 0;
        out->size = 0;
        out->pfn = PFN_NONE;
        out->pad = 0;
    }
}

int a20_madvise_leaf_hold(void *mm_ptr, void *vma_ptr, uintptr_t va,
                          uint32_t pfn, int no_frame_ref, void **out_pcp)
{
    (void)mm_ptr; (void)vma_ptr; (void)va; (void)pfn; (void)no_frame_ref;
    if (out_pcp)
        *out_pcp = NULL;
    return 0;
}

int a20_madvise_demote(void *mm_ptr, uintptr_t va)
{
    (void)mm_ptr;
    (void)va;
    return 0;
}

void a20_madvise_leaf_unmap(void *mm_ptr, uintptr_t va,
                            struct a20_madvise_unmapped *out)
{
    (void)mm_ptr;
    (void)va;
    if (out) {
        out->ok = -1;
        out->base = 0;
        out->size = 0;
        out->pa = 0;
    }
}

void a20_madvise_note_change(void *mm_ptr, uintptr_t base, uintptr_t size)
{
    (void)mm_ptr; (void)base; (void)size;
}

void a20_madvise_drop_cache_ref(void *pcp_ptr)
{
    (void)pcp_ptr;
}

void a20_madvise_release(void *mm_ptr, int kind, void *pcp_ptr, uintptr_t pa,
                         uintptr_t pages)
{
    (void)mm_ptr; (void)kind; (void)pcp_ptr; (void)pa; (void)pages;
}
#endif /* CONFIG_NOMMU */

/* ------------------------------------------------------- ABI-facing API -- */

struct vmo *mm_lookup_vmo_region(mm_struct_t *mm, vaddr_t addr, size_t len,
                                 uint32_t *prot_out)
{
    return a20_madvise_vmo_region(mm, addr, len, prot_out);
}

int mm_madvise_dontneed(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    return a20_madvise_dontneed(mm, addr, len);
}

int mm_madvise(mm_struct_t *mm, vaddr_t addr, size_t len, int advice)
{
    return a20_madvise_dispatch(mm, addr, len, advice);
}

int mm_vma_set_lock(mm_struct_t *mm, vaddr_t start, vaddr_t end, int on)
{
    return a20_madvise_set_lock(mm, start, end, on);
}

