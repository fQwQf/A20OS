#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/pt.h"
#include "mm/mremap_dontunmap_txn.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/vmar.h"
#include "mm/fault.h"
#include "mm/swap.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/errno.h"

/* mremap: relocate/grow/shrink an existing mapping. */

static int mm_remap_placement(mm_struct_t *mm, mm_seg_t *source, size_t len,
                              int flags, vaddr_t hint, int native,
                              vaddr_t *target, int *placement)
{
    struct vmar *owner = source->vmar_owner;
    *target = ((flags & MREMAP_FIXED) || native) ? hint : 0;
    *placement = (flags & MREMAP_FIXED) ? MAP_FIXED : 0;
    if (flags & MREMAP_FIXED) {
        if (owner && (!vmar_contains(owner, *target, len) ||
                      !(owner->cap & VMAR_CAN_MAP_SPECIFIC)))
            return -EACCES;
        return 0;
    }
    if (*target && ((*target & (PAGE_SIZE - 1)) ||
        *target >= USER_VA_LIMIT || len > USER_VA_LIMIT - *target ||
        (owner && !vmar_contains(owner, *target, len)) ||
        mm_range_overlaps(mm, *target, len, NULL)))
        *target = 0;
    if (owner && !*target)
        *target = mm_find_gap(mm, owner->base, len);
    if (owner && (!*target || !vmar_contains(owner, *target, len)))
        return -ENOMEM;
    if (*target) *placement = MAP_FIXED_NOREPLACE;
    return 0;
}

static vaddr_t mm_remap_allocate(mm_struct_t *mm, vaddr_t addr, size_t len,
                                 int prot, int flags)
{
    /* Remap preserves the source lock policy, not the current MCL_FUTURE
     * default. Its net lock charge was checked before entering this phase. */
    uint32_t saved = mm->def_flags;
    mm->def_flags &= ~(uint32_t)VM_LOCKED;
    vaddr_t out = mm_mmap_locked(mm, addr, len, prot, flags);
    mm->def_flags = saved;
    return out;
}

static int mm_remap_copy_backing(mm_struct_t *mm, mm_seg_t *dst,
                                  mm_seg_t *src, vaddr_t addr)
{
    dst->vm_flags = src->vm_flags;
    if (dst->vm_flags & VM_LOCKED) mm->locked_vm += dst->end - dst->start;
    dst->pte_flags = src->pte_flags;
    dst->vmar_cap = src->vmar_cap;
    dst->vmar_owner = src->vmar_owner;
    dst->sysv_shmid = src->sysv_shmid;
    dst->file = src->file;
    dst->file_vnode = src->file_vnode;
    dst->vmo = src->vmo;
    dst->backing_offset = src->backing_offset + (addr - src->start);
    int r = vma_ref_fork(dst);
    if (r < 0) {
        /* vma_ref_fork has already rolled back file references; VMO and
         * placement references are acquired only after the fallible IPC step. */
        dst->file = NULL;
        dst->file_vnode = NULL;
        dst->vmo = NULL;
        dst->vmar_owner = NULL;
        dst->vm_flags &= ~(uint64_t)(VM_VMO | VM_SYSV_SHM);
    }
    return r;
}

static int mm_remap_prepare_unmap(mm_struct_t *mm, vaddr_t start, size_t len)
{
#ifndef CONFIG_NOMMU
    vaddr_t edges[2] = { start, start + len - 1 };
    for (unsigned i = 0; i < 2; i++) {
        int level = 0;
        vaddr_t base = 0;
        size_t size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, edges[i], &level, &base, &size);
        if (pte && (*pte & PTE_V) && level > 0 &&
            (base < start || base + size > start + len)) {
            int r = mm_demote_huge_page(mm, edges[i]);
            if (r < 0) return r;
        }
    }
#else
    (void)start;
#endif
    size_t pages = len / PAGE_SIZE;
    if (pages > SIZE_MAX / (ARCH_PT_LEVELS + 1)) return -ENOMEM;
    return mm_tlb_reserve_holds(mm, pages * (ARCH_PT_LEVELS + 1));
}


/* Pin the same PTE snapshot whose flags are copied. Status-driven COW can
 * replace a base-page leaf without mm->lock, so its node lock is required. */
static int mm_remap_pin_leaf(mm_struct_t *mm, mm_seg_t *vma, vaddr_t va,
                            pte_t *pte, pte_t *snapshot, pfn_t *pfn,
                            page_cache_page_t **page, uint8_t *cls)
{
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    pte_t *table = mm_pt_leaf_table(mm->pgdir, va);
    if (table) mm_pt_node_lock(table);
#else
    (void)mm;
#endif
    *cls = MM_ST_INVALID;
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    *cls = MM_ST_GET_CLASS(mm_pt_status_at(mm->pgdir, va));
#endif
    *snapshot = *pte;
    int r = -EAGAIN;
    *page = NULL;
    if ((*snapshot & PTE_V) && arch_pte_is_leaf(*snapshot)) {
        *pfn = phys_to_pfn(arch_pte_addr(*snapshot));
        r = -ENOMEM;
        if (pfn_valid(*pfn)) {
            *page = mm_file_cache_mapping_get(vma, va, *pfn);
            if (!*page && !(vma->vm_flags & (VM_VMO | VM_PFNMAP))) frame_get(*pfn);
            r = 0;
        }
    }
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    if (table) mm_pt_node_unlock(table);
#endif
    return r;
}

static int mm_clone_shared_mapping(mm_struct_t *mm, mm_seg_t *src_vma,
                                   vaddr_t src_addr, size_t len,
                                   int flags, vaddr_t new_addr,
                                   vaddr_t *out_addr, int reprot, int native) {
    if (!(src_vma->vm_flags & VM_SHARED))
        return -EINVAL;
    if (src_addr + len < src_addr || src_addr + len > src_vma->end)
        return -EINVAL;

#ifdef CONFIG_NOMMU
    return -EOPNOTSUPP;
#endif
    int prot = mm_pte_flags_to_prot(src_vma->pte_flags);
    vaddr_t target;
    int placement;
    int pr = mm_remap_placement(mm, src_vma, len, flags, new_addr,
                                native, &target, &placement);
    if (pr < 0) return pr;
    vaddr_t dst = mm_remap_allocate(mm, target, len, prot,
                           placement | MAP_ANONYMOUS |
                           ((src_vma->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE));
    if (mm_addr_is_error(dst))
        return mm_addr_error(dst);

    /* mm_mmap can merge the new anonymous VMA with neighbors; force exact
     * boundaries before turning it into a file-backed mapping. */
#ifndef CONFIG_NOMMU
    int sr = mm_split_vma_at(mm, dst);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, len);
        return sr;
    }
    sr = mm_split_vma_at(mm, dst + len);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, len);
        return sr;
    }
#endif

    mm_seg_t *dst_vma = mm_seg_find(mm, dst);
    if (!dst_vma || dst_vma->start != dst || dst_vma->end != dst + len) {
        mm_munmap_locked(mm, dst, len);
        return -ENOMEM;
    }

    int br = mm_remap_copy_backing(mm, dst_vma, src_vma, src_addr);
    if (br < 0) {
        mm_munmap_locked(mm, dst, len);
        return br;
    }

    for (vaddr_t off = 0; off < len; ) {
        int level = 0;
        vaddr_t base = 0;
        size_t leaf_size = 0;
        uint64_t src_va = src_addr + off;
        pte_t *src = pt_lookup_leaf(mm->pgdir, src_va, &level, &base, &leaf_size);
#ifdef CONFIG_SWAP
        if (src && pte_is_swap(*src)) {
            int sr = mm_clone_swap_page(mm, dst + off, mm, src_va);
            if (sr == -EAGAIN) continue;
            if (sr < 0) { mm_munmap_locked(mm, dst, len); return sr; }
            off += PAGE_SIZE;
            continue;
        }
#endif
        if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src))
        {
            off += PAGE_SIZE;
            continue;
        }
        if (level > 0 && (base < src_addr || base + leaf_size > src_addr + len ||
                          ((dst + off) & (leaf_size - 1)))) {
            int dr = mm_demote_huge_page(mm, src_va);
            if (dr < 0) {
                mm_munmap_locked(mm, dst, len);
                return dr;
            }
            continue;
        }

        pte_t snapshot;
        pfn_t pfn;
        page_cache_page_t *pcp;
        uint8_t move_cls;
        int pin = mm_remap_pin_leaf(mm, src_vma, src_va, src, &snapshot, &pfn, &pcp, &move_cls);
        if (pin == -EAGAIN) continue;
        if (pin < 0) {
            mm_munmap_locked(mm, dst, len);
            return pin;
        }
        paddr_t pa = arch_pte_addr(snapshot);
        int vmo_owned = (src_vma->vm_flags & (VM_VMO | VM_PFNMAP)) != 0;
        /* Keep the source's class across the move so the copy's status still
         * names the same backing; a huge leaf's slot is level-aware.  The
         * status sidecar is a pgtable-ops feature -- arm32's short-descriptor
         * backend has none and NOMMU has no table at all -- so where it is
         * absent the move starts from MM_ST_INVALID and takes the default
         * below, the same answer the query gives when the slot is unreadable. */
        if (move_cls == MM_ST_INVALID || move_cls == MM_ST_PT_NODE)
            move_cls = MM_ST_ANON_MAPPED;
        int r = (level > 0) ? pt_map_huge(mm, dst + off, pa,
                                          arch_pte_flags(snapshot), move_cls)
                            : pt_map(mm->pgdir, dst + off, pa,
                                     arch_pte_flags(snapshot));
        if (r < 0) {
            if (pcp) {
                page_cache_put(pcp);
            } else if (!vmo_owned) {
                frame_put(pfn);
            }
            mm_munmap_locked(mm, dst, len);
            return r;
        }
        mm_rss_add(mm, leaf_size / PAGE_SIZE);
        off += leaf_size;
    }

    if (reprot >= 0) {
        int r = mm_mprotect_locked(mm, dst, len, reprot);
        if (r < 0) {
            mm_munmap_locked(mm, dst, len);
            return r;
        }
    }
    *out_addr = dst;
    return 0;
}

static __attribute__((unused)) int mm_move_mapping_pages(mm_struct_t *mm, vaddr_t old_addr,
                                 vaddr_t dst, size_t len, int dontunmap) {
#if !defined(CONFIG_MM_DONTUNMAP_TEST_FAIL_AFTER_MAP_COUNT)
    (void)dontunmap;
#endif
#if defined(CONFIG_MM_DONTUNMAP_TEST_FAIL_AFTER_MAP_COUNT)
    size_t test_mapped_leaves = 0;
    static int inject_once = 1;
#endif
    for (vaddr_t off = 0; off < len; ) {
        int level = 0;
        vaddr_t base = 0;
        size_t leaf_size = 0;
        uint64_t src_va = old_addr + off;
        pte_t *src = pt_lookup_leaf(mm->pgdir, src_va, &level, &base, &leaf_size);
#ifdef CONFIG_SWAP
        if (src && pte_is_swap(*src)) {
            int sr = mm_clone_swap_page(mm, dst + off, mm, src_va);
            if (sr == -EAGAIN) continue;
            if (sr < 0) return sr;
            off += PAGE_SIZE;
            continue;
        }
#endif
        if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src))
        {
            off += PAGE_SIZE;
            continue;
        }
        if (level > 0 && (base < old_addr || base + leaf_size > old_addr + len ||
                          ((dst + off) & (leaf_size - 1)))) {
            int dr = mm_demote_huge_page(mm, src_va);
            if (dr < 0)
                return dr;
            continue;
        }

        mm_seg_t *src_vma = mm_seg_find(mm, src_va);
        if (!src_vma) return -EFAULT;
        pte_t snapshot;
        pfn_t pfn;
        page_cache_page_t *pcp;
        uint8_t move_cls;
        int pin = mm_remap_pin_leaf(mm, src_vma, src_va, src, &snapshot, &pfn, &pcp, &move_cls);
        if (pin == -EAGAIN) continue;
        if (pin < 0) return pin;
        paddr_t pa = arch_pte_addr(snapshot);
        uint64_t pte_flags = arch_pte_flags(snapshot);
        int vmo_owned = (src_vma->vm_flags & (VM_VMO | VM_PFNMAP)) != 0;
        if (move_cls == MM_ST_INVALID || move_cls == MM_ST_PT_NODE)
            move_cls = MM_ST_ANON_MAPPED;
        int r = (level > 0) ? pt_map_huge(mm, dst + off, pa, pte_flags,
                                          move_cls)
                            : pt_map(mm->pgdir, dst + off, pa, pte_flags);
        if (r < 0) {
            if (pcp) {
                page_cache_put(pcp);
            } else if (!vmo_owned) {
                frame_put(pfn);
            }
            return r;
        }

        /* Keep every source PTE and mapping reference until the entire
         * destination (and optional new protection) has succeeded. */
        mm_rss_add(mm, leaf_size / PAGE_SIZE);
        off += leaf_size;
#if defined(CONFIG_MM_DONTUNMAP_TEST_FAIL_AFTER_MAP_COUNT)
        /* Build-only deterministic rollback probe.  This compile-time hook is
         * never enabled in normal images and fails after the configured number
         * of successful destination leaves, before source teardown begins. */
        if (dontunmap &&
            ++test_mapped_leaves == CONFIG_MM_DONTUNMAP_TEST_FAIL_AFTER_MAP_COUNT &&
            inject_once) {
            inject_once = 0;
            klog_write("MM_DONTUNMAP_TEST: injected-failure leaves=%lu\n",
                       (unsigned long)test_mapped_leaves);
            return -ENOMEM;
        }
#endif
    }
    return 0;
}

/* DONTUNMAP is committed only after the destination has been completely
 * populated and protected.  The source VMA stays in place, but its present
 * and swapped entries must be dropped so later accesses take the original
 * VMA's normal fault path.  All fallible destination work has already
 * completed; the enclosing remap reserved enough TLB holds before creating
 * the destination, so this source teardown has no recoverable failure point.
 */
#ifndef CONFIG_NOMMU
static int mm_mremap_dontunmap_preflight(mm_struct_t *mm, vaddr_t start,
                                         size_t len)
{
    vaddr_t end = start + len;
    for (vaddr_t va = start; va < end;) {
        int level = 0;
        vaddr_t base = 0;
        size_t leaf_size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &leaf_size);
        if (pte && (*pte & PTE_V) && arch_pte_is_leaf(*pte) && level > 0 &&
            (base < start || base + leaf_size > end))
            return -EINVAL;
        vaddr_t next = leaf_size ? base + leaf_size : va + PAGE_SIZE;
        va = next > va ? next : va + PAGE_SIZE;
    }
    return 0;
}

static void mm_mremap_dontunmap_clear_source(mm_struct_t *mm, vaddr_t start,
                                             size_t len)
{
    vaddr_t end = start + len;
    for (vaddr_t va = start; va < end;) {
        int level = 0;
        vaddr_t base = 0;
        size_t leaf_size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &leaf_size);
#ifdef CONFIG_SWAP
        if (pte && pte_is_swap(*pte)) {
            if (pt_unmap_leaf(mm, va, NULL, &base, &leaf_size, NULL) == 0) {
                mm_tlb_note_change(mm, base, leaf_size);
                va = base + leaf_size;
            }
            /* A concurrent page-in may have replaced the swap entry.  Retry
             * this address and discard whichever mapping is current. */
            continue;
        }
#endif
        if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) {
            va += PAGE_SIZE;
            continue;
        }

        mm_seg_t *vma = mm_seg_find(mm, va);
        if (!vma || va < vma->start || va >= vma->end)
            panic("mremap: DONTUNMAP source lost VMA va=%p", (void *)va);

        if ((vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED) &&
            vma->file_vnode && (*pte & PTE_D)) {
            uint64_t index = vma->backing_offset + (va - vma->start);
            page_cache_page_t *dirty = page_cache_get(vma->file_vnode,
                                                       index / PAGE_SIZE, 0);
            if (dirty) {
                page_cache_mark_dirty(dirty);
                page_cache_put(dirty);
            }
        }

        paddr_t pa = 0;
        int ur = pt_unmap_leaf(mm, va, &pa, &base, &leaf_size, NULL);
        if (ur < 0) {
            /* The status-driven COW/page-in paths can replace a PTE without
             * mm->lock.  In that case re-read and discard the replacement. */
            continue;
        }
        mm_tlb_note_change(mm, base, leaf_size);
        if (pa) {
            pfn_t pfn = phys_to_pfn(pa);
            page_cache_page_t *pcp = mm_file_cache_mapping_get(vma, va, pfn);
            if (pcp) {
                if (mm_tlb_hold_page(mm, pcp) < 0)
                    panic("mremap: reserved DONTUNMAP page hold unavailable");
                /* pcp owns the cache lookup reference; the old source PTE
                 * owned one more.  The queued TLB hold survives both puts. */
                page_cache_put(pcp);
                page_cache_put(pcp);
            } else if (!(vma->vm_flags & (VM_PFNMAP | VM_VMO))) {
                if (!pfn_valid(pfn) || mm_tlb_hold_frame(mm, pfn) < 0)
                    panic("mremap: reserved DONTUNMAP frame hold unavailable");
                frame_put(pfn);
            }
            mm_rss_sub_clamped(mm, leaf_size / PAGE_SIZE);
        }
        va = base + leaf_size;
    }
}

/* Primitive adapters used by the selectable C/Rust DONTUNMAP transaction.
 * Each callback performs one existing MM operation; the transaction module
 * owns sequencing and rollback, rather than hiding the old C sequence behind
 * one callback. */
typedef struct mm_dontunmap_context {
    mm_struct_t *mm;
    mm_seg_t *source_vma;
    vaddr_t source;
    size_t source_len;
    vaddr_t destination;
    size_t destination_len;
    int reprot;
} mm_dontunmap_context_t;

#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
#ifdef CONFIG_MREMAP_DONTUNMAP_TXN_RUST
#define DONTUNMAP_TEST_IMPL "rust"
#else
#define DONTUNMAP_TEST_IMPL "c"
#endif
static void dontunmap_test_txn_marker(const char *phase, int exact, int rc)
{
    klog_write("MM_DONTUNMAP_TXN_TEST: impl=%s phase=%s exact=%d rc=%d\n",
               DONTUNMAP_TEST_IMPL, phase, exact, rc);
}
#else
#define dontunmap_test_txn_marker(phase, exact, rc) do { \
    (void)(phase); (void)(exact); (void)(rc); \
} while (0)
#endif

static int dontunmap_split_boundary(mm_struct_t *mm, vaddr_t addr,
                                   unsigned int split_call)
{
#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
    return mm_split_vma_at_dontunmap_test(mm, addr, split_call);
#else
    (void)split_call;
    return mm_split_vma_at(mm, addr);
#endif
}

static int dontunmap_prepare_destination(void *opaque)
{
    mm_dontunmap_context_t *ctx = opaque;
    dontunmap_test_txn_marker("prepare-enter", 0, 0);
    int r = dontunmap_split_boundary(ctx->mm, ctx->destination, 1);
    if (r < 0) {
        dontunmap_test_txn_marker("prepare-error", 0, r);
        return r;
    }
    r = dontunmap_split_boundary(ctx->mm,
                                 ctx->destination + ctx->destination_len, 2);
    if (r < 0) {
        dontunmap_test_txn_marker("prepare-error", 0, r);
        return r;
    }
    mm_seg_t *vma = mm_seg_find(ctx->mm, ctx->destination);
    if (!vma || vma->start != ctx->destination ||
        vma->end != ctx->destination + ctx->destination_len)
        return -ENOMEM;

    /* mm_insert_vma may have merged an adjacent anonymous destination into
     * the source and deferred the source's old VMA record. Rebuild the source
     * boundaries and reacquire by address before any callback dereferences it. */
    r = dontunmap_split_boundary(ctx->mm, ctx->source, 3);
    if (r < 0) {
        dontunmap_test_txn_marker("prepare-error", 0, r);
        return r;
    }
    r = dontunmap_split_boundary(ctx->mm, ctx->source + ctx->source_len, 4);
    if (r < 0) {
        dontunmap_test_txn_marker("prepare-error", 0, r);
        return r;
    }
    ctx->source_vma = mm_seg_find(ctx->mm, ctx->source);
    if (!ctx->source_vma || ctx->source_vma->start != ctx->source ||
        ctx->source_vma->end != ctx->source + ctx->source_len)
        return -EFAULT;
    dontunmap_test_txn_marker("prepare-ready", 0, 0);
    return 0;
}

static int dontunmap_copy_backing(void *opaque)
{
    mm_dontunmap_context_t *ctx = opaque;
    mm_seg_t *dst_vma = mm_seg_find(ctx->mm, ctx->destination);
    if (!dst_vma || dst_vma->start != ctx->destination ||
        dst_vma->end != ctx->destination + ctx->destination_len)
        return -ENOMEM;
    return mm_remap_copy_backing(ctx->mm, dst_vma, ctx->source_vma,
                                 ctx->source);
}

static int dontunmap_move_pages(void *opaque)
{
    mm_dontunmap_context_t *ctx = opaque;
    return mm_move_mapping_pages(ctx->mm, ctx->source, ctx->destination,
                                 ctx->source_len, 1);
}

static int dontunmap_reprotect(void *opaque)
{
    mm_dontunmap_context_t *ctx = opaque;
    return ctx->reprot >= 0
               ? mm_mprotect_locked(ctx->mm, ctx->destination,
                                    ctx->destination_len, ctx->reprot)
               : 0;
}

static int dontunmap_preflight_source(void *opaque)
{
    mm_dontunmap_context_t *ctx = opaque;
    return mm_mremap_dontunmap_preflight(ctx->mm, ctx->source,
                                         ctx->source_len);
}

static void dontunmap_clear_source(void *opaque)
{
    mm_dontunmap_context_t *ctx = opaque;
    mm_mremap_dontunmap_clear_source(ctx->mm, ctx->source,
                                     ctx->source_len);
}

static void dontunmap_release_source_lock(void *opaque)
{
    mm_dontunmap_context_t *ctx = opaque;
    if (ctx->source_vma->vm_flags & VM_LOCKED) {
        ctx->source_vma->vm_flags &= ~(uint64_t)VM_LOCKED;
        ctx->mm->locked_vm = ctx->mm->locked_vm >= ctx->source_len
                                 ? ctx->mm->locked_vm - ctx->source_len : 0;
    }
}

static int dontunmap_rollback_destination(void *opaque, int exact_vma)
{
    mm_dontunmap_context_t *ctx = opaque;
    dontunmap_test_txn_marker("rollback-begin", exact_vma, 0);
    if (exact_vma) {
        mm_seg_t *vma = mm_seg_find(ctx->mm, ctx->destination);
        if (!vma || vma->start != ctx->destination ||
            vma->end != ctx->destination + ctx->destination_len) {
            dontunmap_test_txn_marker("rollback-end", exact_vma, -EFAULT);
            return -EFAULT;
        }
    }
    int r = mm_munmap_locked(ctx->mm, ctx->destination,
                             ctx->destination_len);
    dontunmap_test_txn_marker("rollback-end", exact_vma, r);
    return r;
}

static void dontunmap_rollback_failed(void *opaque, int primary_error,
                                      int cleanup_error)
{
    mm_dontunmap_context_t *ctx = opaque;
    klog_write("MM_DONTUNMAP_TXN: rollback failed primary=%d cleanup=%d dst=%p len=%lu\n",
               primary_error, cleanup_error, (void *)ctx->destination,
               (unsigned long)ctx->destination_len);
}

static const mm_dontunmap_txn_ops_t dontunmap_txn_ops = {
    .prepare_destination = dontunmap_prepare_destination,
    .copy_backing = dontunmap_copy_backing,
    .move_pages = dontunmap_move_pages,
    .reprotect = dontunmap_reprotect,
    .preflight_source = dontunmap_preflight_source,
    .clear_source = dontunmap_clear_source,
    .release_source_lock = dontunmap_release_source_lock,
    .rollback_destination = dontunmap_rollback_destination,
    .rollback_failed = dontunmap_rollback_failed,
};

static int mm_mremap_dontunmap_transaction(mm_struct_t *mm,
                                           vaddr_t source, size_t source_len,
                                           vaddr_t destination,
                                           size_t destination_len, int reprot,
                                           vaddr_t *out_destination)
{
    mm_dontunmap_context_t ctx = {
        .mm = mm,
        .source_vma = NULL,
        .source = source,
        .source_len = source_len,
        .destination = destination,
        .destination_len = destination_len,
        .reprot = reprot,
    };
    uintptr_t out = 0;
#ifdef CONFIG_MREMAP_DONTUNMAP_TXN_RUST
    dontunmap_test_txn_marker("enter", -1, 0);
    int r = mm_mremap_dontunmap_txn_rust(&dontunmap_txn_ops, &ctx,
                                         destination, destination_len, &out);
#else
    dontunmap_test_txn_marker("enter", -1, 0);
    int r = mm_mremap_dontunmap_txn_c(&dontunmap_txn_ops, &ctx,
                                      destination, destination_len, &out);
#endif
    if (r == 0)
        *out_destination = (vaddr_t)out;
    return r;
}
#endif

static int mm_mremap_impl_locked(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
                    size_t new_size, int flags, vaddr_t new_addr,
                    vaddr_t *out_addr, int reprot, int native) {
    if (!mm || !out_addr) return -EINVAL;
    if (!new_size || old_size > SIZE_MAX - (PAGE_SIZE - 1) ||
        new_size > SIZE_MAX - (PAGE_SIZE - 1)) return -EINVAL;
    if (old_addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
        return -EINVAL;
    if ((flags & MREMAP_FIXED) &&
        (!(flags & MREMAP_MAYMOVE) || (new_addr & (PAGE_SIZE - 1))))
        return -EINVAL;
    if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
        return -EINVAL;
    mm_seg_index_invalidate(mm);

    size_t old_len = ROUND_UP(old_size, PAGE_SIZE);
    size_t new_len = ROUND_UP(new_size, PAGE_SIZE);
    if (new_len == 0) return -EINVAL;
    if (old_addr + old_len < old_addr || old_addr + new_len < old_addr)
        return -EINVAL;
    if ((flags & MREMAP_FIXED) && new_addr + new_len < new_addr)
        return -EINVAL;

    mm_seg_t *vma = mm_seg_find(mm, old_addr);
    if (!vma || old_addr < vma->start || old_addr >= vma->end) return -EFAULT;
    if (vma->vm_flags & VM_SEALED) return -EPERM;
#ifdef CONFIG_NOMMU
    if (flags & MREMAP_DONTUNMAP) return -EOPNOTSUPP;
#else
    /* Raw PFN mappings have no ordinary VMA fault source to recreate a
     * cleared entry.  Linux rejects DONTUNMAP for VM_PFNMAP for this reason. */
    if ((flags & MREMAP_DONTUNMAP) && (vma->vm_flags & VM_PFNMAP))
        return -EINVAL;
#endif
    int force_copy = reprot >= 0 && reprot != mm_pte_flags_to_prot(vma->pte_flags);
    if (reprot >= 0 && ((reprot & ~7) ||
        ((vma->vm_flags & VM_NATIVE_CAP) && ((uint32_t)reprot & ~vma->vmar_cap))))
        return -EACCES;
    size_t extra = (old_size == 0 || (flags & MREMAP_DONTUNMAP)) ? new_len :
                   (new_len > old_len ? new_len - old_len : 0);
    task_t *cur = proc_current();
    if (cur && cur->mm == mm) {
        uint64_t limit = cur->limits.as / PAGE_SIZE;
        if (extra && cur->limits.as &&
            (mm->total_vm > limit || extra / PAGE_SIZE > limit - mm->total_vm))
            return -ENOMEM;
        if (extra && (vma->vm_flags & VM_LOCKED) &&
            !proc_has_cap(cur, CAP_SYS_ADMIN) &&
            (mm->locked_vm > cur->limits.memlock ||
             extra > cur->limits.memlock - mm->locked_vm))
            return -ENOMEM;
    }
    /* SysV's unmap contract does not permit partial segment teardown. Do not
     * split one attachment into VMAs whose independent releases undercount it. */
    if (old_size && (vma->vm_flags & VM_SYSV_SHM) &&
        (old_addr != vma->start || old_len != vma->end - vma->start || new_len != old_len))
        return -EINVAL;


    /* mseal(2): remapping (move/shrink/grow or DONTUNMAP) of a sealed VMA is
     * refused. */
    {
        vaddr_t chk_end = old_addr + old_len;
        for (mm_seg_t *v = mm_seg_find(mm, old_addr); v && v->start < chk_end;
             v = v->next) {
            if (v->start >= chk_end || v->end <= old_addr)
                continue;
            if (v->vm_flags & VM_SEALED)
                return -EPERM;
        }
    }

    if (old_size == 0) {
        if (!(flags & MREMAP_MAYMOVE) || (flags & MREMAP_DONTUNMAP))
            return -EINVAL;
        if ((flags & MREMAP_FIXED) &&
            new_addr < old_addr + new_len && old_addr < new_addr + new_len)
            return -EINVAL;
        size_t pages = new_len / PAGE_SIZE;
        if (pages > SIZE_MAX / (2 * (ARCH_PT_LEVELS + 1))) return -ENOMEM;
        int hr = mm_tlb_reserve_holds(mm, pages * 2 * (ARCH_PT_LEVELS + 1));
        if (hr < 0) return hr;
        return mm_clone_shared_mapping(mm, vma, old_addr, new_len, flags,
                                       new_addr, out_addr, reprot, native);
    }

    if (old_len == 0) return -EINVAL;
    if (old_addr + old_len < old_addr || old_addr + old_len > vma->end)
        return -EFAULT;
    if ((vma->vm_flags & VM_VMO) && vma->vmo) {
        uint64_t offset = vma->backing_offset + (old_addr - vma->start);
        if (offset < vma->backing_offset || offset > vma->vmo->size ||
            new_len > vma->vmo->size - offset)
            return -EINVAL;
    }
    if ((flags & MREMAP_DONTUNMAP) && old_len != new_len)
        return -EINVAL;

#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
    int r = (flags & MREMAP_DONTUNMAP)
        ? mm_split_vma_at_dontunmap_test(mm, old_addr, 5)
        : mm_split_vma_at(mm, old_addr);
#else
    int r = mm_split_vma_at(mm, old_addr);
#endif
    if (r < 0) {
#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
        if (flags & MREMAP_DONTUNMAP)
            klog_write("MM_DONTUNMAP_TXN_TEST: phase=source-presplit-error split=5 rc=%d\n", r);
#endif
        return r;
    }
#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
    r = (flags & MREMAP_DONTUNMAP)
        ? mm_split_vma_at_dontunmap_test(mm, old_addr + old_len, 6)
        : mm_split_vma_at(mm, old_addr + old_len);
#else
    r = mm_split_vma_at(mm, old_addr + old_len);
#endif
    if (r < 0) {
#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
        if (flags & MREMAP_DONTUNMAP)
            klog_write("MM_DONTUNMAP_TXN_TEST: phase=source-presplit-error split=6 rc=%d\n", r);
#endif
        return r;
    }
    vma = mm_seg_find(mm, old_addr);
    if (!vma || vma->start != old_addr || vma->end < old_addr + old_len)
        return -EFAULT;

    if (native && new_len == old_len &&
        !(flags & (MREMAP_DONTUNMAP | MREMAP_FIXED))) {
        if (reprot >= 0) {
            r = mm_mprotect_locked(mm, old_addr, old_len, reprot);
            if (r < 0) return r;
        }
        *out_addr = old_addr;
        return 0;
    }

    if (!force_copy && new_len <= old_len && !(flags & (MREMAP_DONTUNMAP | MREMAP_FIXED))) {
        if (new_len < old_len) {
            r = mm_remap_prepare_unmap(mm, old_addr + new_len, old_len - new_len);
            if (r < 0) return r;
            r = mm_munmap_locked(mm, old_addr + new_len, old_len - new_len);
            if (r < 0) return r;
        }
        *out_addr = old_addr;
        return 0;
    }

    if (!force_copy && new_len > old_len && !(flags & MREMAP_DONTUNMAP)) {
        vaddr_t new_end = old_addr + new_len;
        int can_grow = !(flags & MREMAP_FIXED) &&
                       new_end <= USER_VA_LIMIT &&
                       (!vma->vmar_owner || vmar_contains(vma->vmar_owner, old_addr, new_len));
        if (old_addr + old_len != vma->end)
            can_grow = 0;
        if (can_grow && !mm_range_overlaps(mm, old_addr + old_len,
                                           new_len - old_len, vma)) {
            vma->end = new_end;
            mm->total_vm += (new_len - old_len) / PAGE_SIZE;
            if (vma->vm_flags & VM_LOCKED) mm->locked_vm += new_len - old_len;
            vma_try_merge(mm, vma);
            *out_addr = old_addr;
            return 0;
        }
    }

    if (!(flags & MREMAP_MAYMOVE))
        return -ENOMEM;

    if ((flags & MREMAP_FIXED) &&
        new_addr < old_addr + old_len && old_addr < new_addr + new_len)
        return -EINVAL;

    /* One frame/cache hold and at most ARCH_PT_LEVELS retired table records
     * per page cover source commit or destination rollback. Include both spans
     * for fixed-target replacement and huge-leaf demotion bookkeeping. */
    size_t pages = old_len / PAGE_SIZE + new_len / PAGE_SIZE;
    if (pages > SIZE_MAX / (ARCH_PT_LEVELS + 1)) return -ENOMEM;
    int hr = mm_tlb_reserve_holds(mm, pages * (ARCH_PT_LEVELS + 1));
    if (hr < 0) return hr;

    int prot = mm_pte_flags_to_prot(vma->pte_flags);
    vaddr_t target;
    int placement;
    int pr = mm_remap_placement(mm, vma, new_len, flags, new_addr,
                                native, &target, &placement);
    if (pr < 0) return pr;
    vaddr_t dst = mm_remap_allocate(mm, target, new_len, prot,
                           placement | MAP_ANONYMOUS |
                           ((vma->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE));
    if (mm_addr_is_error(dst))
        return mm_addr_error(dst);

    if (flags & MREMAP_DONTUNMAP) {
#ifndef CONFIG_NOMMU
        /* The destination is now allocated. The selected transaction owns it
         * through split/backing/copy/reprotect/preflight and either source
         * commit or rollback; the other mremap modes remain on the C path. */
        return mm_mremap_dontunmap_transaction(mm, old_addr, old_len,
                                               dst, new_len, reprot, out_addr);
#endif
    }

    /* mm_mmap can merge the new anonymous VMA with neighbors; force exact
     * boundaries before turning it into a file-backed mapping. */
    int sr = mm_split_vma_at(mm, dst);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return sr;
    }
    sr = mm_split_vma_at(mm, dst + new_len);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return sr;
    }

    mm_seg_t *dst_vma = mm_seg_find(mm, dst);
    if (!dst_vma || dst_vma->start != dst || dst_vma->end != dst + new_len) {
        mm_munmap_locked(mm, dst, new_len);
        return -ENOMEM;
    }

    int br = mm_remap_copy_backing(mm, dst_vma, vma, old_addr);
    if (br < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return br;
    }

    size_t move_len = old_len < new_len ? old_len : new_len;
#ifdef CONFIG_NOMMU
    memcpy((void *)dst, (void *)old_addr, move_len);
    r = 0;
#else
    r = mm_move_mapping_pages(mm, old_addr, dst, move_len, 0);
#endif
    if (r < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return r;
    }

    if (reprot >= 0) {
        r = mm_mprotect_locked(mm, dst, new_len, reprot);
        if (r < 0) {
            mm_munmap_locked(mm, dst, new_len);
            return r;
        }
    }
    r = mm_munmap_locked(mm, old_addr, old_len);
    if (r < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return r;
    }

    *out_addr = dst;
    return 0;
}

int mm_mremap_locked(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
                    size_t new_size, int flags, vaddr_t new_addr,
                    vaddr_t *out_addr)
{
    return mm_mremap_impl_locked(mm, old_addr, old_size, new_size, flags,
                                 new_addr, out_addr, -1, 0);
}

int mm_mremap_native_locked(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
                     size_t new_size, vaddr_t hint, int prot, vaddr_t *out_addr)
{
    return mm_mremap_impl_locked(mm, old_addr, old_size, new_size,
                                 MREMAP_MAYMOVE, hint, out_addr, prot ? prot : -1, 1);
}

int mm_mremap(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
              size_t new_size, int flags, vaddr_t new_addr,
              vaddr_t *out_addr)
{
    if (!mm) return -EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    int r = mm_mremap_locked(mm, old_addr, old_size, new_size, flags,
                             new_addr, out_addr);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_tlb_invalidate_finish(mm);
    return r;
}
