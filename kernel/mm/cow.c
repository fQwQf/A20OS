#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/pt.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/swap.h"
#include "fs/page_cache.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/perf.h"
#include "core/timer.h"

#ifndef CONFIG_NOMMU

/*
 * Fork / copy-on-write page-table cloning.
 *
 * mm_fork() (in mm/vm.c) drives the VMA snapshot and calls these helpers while
 * holding parent->lock; status-driven faults also require leaf-node exclusion.
 * The clone walks only already-present leaf PTEs; file-backed shared mappings
 * are left populated on demand.
 */

static size_t vm_pt_level_size(int level) {
    return PAGE_SIZE << (ARCH_PT_BITS * level);
}

uint64_t mm_cow_flags(uint64_t pte) {
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
static uint8_t mm_fork_page_class(pte_t *table, int level, vaddr_t va,
                                 const mm_seg_t *v)
{
    /* The snapshot already holds this leaf's node lock. Use its exact level
     * (huge leaves have their own status slot), not another root descent. */
    uint8_t cls = table
                      ? MM_ST_GET_CLASS(mm_pt_peek(table, level,
                                                   arch_pt_vpn(va, level)))
                      : MM_ST_INVALID;
    if (cls != MM_ST_INVALID && cls != MM_ST_PT_NODE)
        return cls;
    /* Recover missing backing metadata from the VMA the same snapshot has
     * already resolved under parent->lock; no second VMA lookup is needed. */
    if (!v)
        return MM_ST_ANON_MAPPED;
    if (v->vm_flags & VM_VMO)
        return MM_ST_VMO;
    if (v->vm_flags & VM_FILE)
        return (v->vm_flags & VM_SHARED) ? MM_ST_FILE_SHARED
                                         : MM_ST_FILE_PRIVATE;
    return MM_ST_ANON_MAPPED;
}
#else
/* The short-descriptor backend has no status sidecar, as before. */
static uint8_t mm_fork_page_class(pte_t *table, int level, vaddr_t va,
                                 const mm_seg_t *v)
{
    (void)table;
    (void)level;
    (void)va;
    (void)v;
    return MM_ST_ANON_MAPPED;
}
#endif /* ARCH_HAS_PGTABLE_OPS */

#ifdef CONFIG_SWAP
int mm_clone_swap_page(mm_struct_t *dst, vaddr_t dst_va,
                        mm_struct_t *src, vaddr_t src_va)
{
    pte_t *pte = pt_lookup_leaf(src->pgdir, src_va, NULL, NULL, NULL);
    if (!pte) return -EAGAIN;
#if defined(ARCH_HAS_PGTABLE_OPS)
    pte_t *table = mm_pt_leaf_table(src->pgdir, src_va);
    if (table) mm_pt_node_lock(table);
#endif
    pte_t snapshot = *pte;
    int r = -EAGAIN;
    swap_entry_t entry = 0;
    if (pte_is_swap(snapshot)) {
        entry = pte_to_swp_entry(snapshot);
        r = swap_duplicate(entry);
    }
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (table) mm_pt_node_unlock(table);
#endif
    if (r < 0) return r;
    mm_seg_t *vma = mm_seg_find(dst, dst_va);
    if (!vma || dst_va < vma->start || dst_va >= vma->end) {
        swap_free(entry);
        return -EFAULT;
    }
    pte_t prot = vma ? vma->pte_flags : 0;
#if defined(ARCH_HAS_PGTABLE_OPS)
    mm_cursor_t cur;
    r = mm_addrspace_lock(dst, dst_va, dst_va + PAGE_SIZE, &cur);
    if (r == 0) {
        r = mm_cursor_map_swap(&cur, dst_va, entry, prot);
        mm_cursor_unlock(&cur);
    } else if (r > 0) r = -EFAULT;
#else
    (void)prot;
    pte_t *target = pt_walk(dst->pgdir, dst_va, 1);
    r = target ? (*target ? -EEXIST : 0) : -ENOMEM;
    if (!r) *target = swp_entry_to_pte(entry);
#endif
    if (r < 0) swap_free(entry);
    return r;
}
#endif

/*
 * The fork/COW clone transaction lives in kernel/mm/cow_clone.rs.  What is
 * left here is the page-table walker (below), the leaf probe, the node lock
 * and status sidecar, the PTE encoders, pt_map/pt_map_huge, the frame
 * allocator, the page-cache lookup and the swap-entry duplication.
 */
extern int a20_fork_clone_page_rust(mm_struct_t *child, mm_struct_t *parent,
                                    vaddr_t va, int shared);
extern int a20_cow_clone_leaf_rust(mm_struct_t *child, mm_struct_t *parent,
                                   vaddr_t va, int level, int shared);
extern int a20_cow_clone_leaf_at_rust(mm_struct_t *child, mm_struct_t *parent,
                                      vaddr_t va, int level, int shared,
                                      int checked, pte_t *slot, pte_t *node,
                                      mm_seg_t *vma);
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
extern int a20_cow_clone_anon_batch_rust(mm_struct_t *child,
                                         mm_struct_t *parent, vaddr_t va,
                                         pte_t *node, size_t nr, int shared);
#endif

struct a20_cow_src {
    void     *pcp;
    uintptr_t pte;
    uintptr_t base;
    uintptr_t size;
    uintptr_t pa;
    uintptr_t raw_flags;
    uintptr_t cow_flags;
    uint32_t  pfn;
    int32_t   level;
    int32_t   present;
    int32_t   swap;
    int32_t   is_leaf;
    int32_t   w_or_cow;
    uint32_t  class;
    int32_t   kind;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_cow_src) == 88, "Rust/C cow src ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct a20_cow_src) == 60, "Rust/C cow src ABI");
#endif

/* The pinned share transaction carries the exact slot and locked table;
 * downgrade/status/unlock must not walk from the root again to rediscover
 * these. parent->lock keeps their topology/lifetime stable throughout. */
struct a20_cow_shared_snapshot {
    void *pcp;
    pte_t *slot;
    pte_t *node;
    uintptr_t pa;
    uintptr_t raw_flags;
    uintptr_t cow_flags;
    uint32_t pfn;
    int32_t w_or_cow;
    uint32_t class;
    int32_t kind;
};
_Static_assert(sizeof(struct a20_cow_shared_snapshot) ==
               6 * sizeof(uintptr_t) + 16, "Rust/C cow shared snapshot ABI");

uint64_t a20_cow_policy_flags(const void *parent_ptr, uintptr_t va, int *present)
{
    const mm_struct_t *parent = parent_ptr;
    mm_seg_t *policy = mm_seg_find((mm_struct_t *)parent, va);
    if (!policy || va < policy->start || va >= policy->end) {
        if (present)
            *present = 0;
        return 0;
    }
    if (present)
        *present = 1;
    return policy->vm_flags;
}

void a20_cow_probe(const void *parent_ptr, uintptr_t va,
                   struct a20_cow_src *out)
{
    const mm_struct_t *parent = parent_ptr;
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->pfn = PFN_NONE;
    /* The struct fields are the width-fixed Rust ABI (int32_t/uintptr_t);
     * pt_lookup_leaf() takes the kernel's native int/vaddr_t.  Those are the
     * same width but not the same C type on a 32-bit target, so the call goes
     * through locals rather than taking the struct members' addresses. */
    int level = 0;
    vaddr_t base = 0;
    pte_t *src = pt_lookup_leaf(parent->pgdir, va, &level, &base, &out->size);
    out->level = level;
    out->base = base;
    if (!src)
        return;
    out->pte = (uintptr_t)*src;
    out->present = (*src & PTE_V) != 0;
#ifdef CONFIG_SWAP
    out->swap = pte_is_swap(*src);
#endif
    out->is_leaf = arch_pte_is_leaf(*src);
    if (out->present) {
        out->pa = arch_pte_addr(*src);
        out->pfn = phys_to_pfn(out->pa);
    }
    out->raw_flags = arch_pte_flags(*src);
    out->cow_flags = mm_cow_flags(*src);
    out->w_or_cow = ((*src) & (PTE_W | PTE_COW)) != 0;
}

int a20_cow_dst_present(const void *child_ptr, uintptr_t va)
{
    const mm_struct_t *child = child_ptr;
    pte_t *dst = pt_lookup_leaf(child->pgdir, va, NULL, NULL, NULL);
    return dst && (*dst & PTE_V);
}

int a20_cow_clone_swap(void *child, void *parent, uintptr_t va)
{
#ifdef CONFIG_SWAP
    return mm_clone_swap_page(child, va, parent, va);
#else
    (void)child;
    (void)parent;
    (void)va;
    return -EAGAIN;
#endif
}

void a20_cow_geometry(int level, uintptr_t *size, uintptr_t *pmd_size,
                      int *pmd_order)
{
    if (size)
        *size = vm_pt_level_size(level);
    if (pmd_size)
        *pmd_size = PMD_SIZE;
    if (pmd_order)
        *pmd_order = PMD_ORDER;
}

uint32_t a20_cow_frame_alloc(int order)
{
    return pfa_alloc(order);
}

void a20_cow_frame_free(uint32_t pfn, int order)
{
    pfa_free(pfn, order);
}

void a20_cow_frame_copy(uint32_t dst, uint32_t src, uintptr_t size)
{
    memcpy(pfn_to_virt(dst), pfn_to_virt(src), size);
}

int a20_cow_map_child(void *child_ptr, uintptr_t va, uint32_t pfn,
                      uintptr_t flags, uint32_t class, int level, void *pcp)
{
    mm_struct_t *child = child_ptr;
    paddr_t pa = pfn_to_phys(pfn);
    if (level > 0)
        return pt_map_huge(child, va, pa, (pte_t)flags, (uint8_t)class);
    /* Preserve source backing metadata for both 4K and huge leaves. */
    return pt_map_cache_cls(child->pgdir, va, pa, (pte_t)flags,
                            (uint8_t)class, pcp);
}

/* Narrow bridge from the Rust batch transaction to the child page-table
 * mapper.  Its all-or-none contract lets Rust transfer the whole batch's
 * source pins only after every child leaf is installed. */
int a20_cow_map_child_anon_batch(
    void *child_ptr, uintptr_t va,
    const struct a20_cow_anon_batch_page *pages, size_t nr, int shared)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    mm_struct_t *child = child_ptr;
    if (!child || !child->pgdir)
        return -EINVAL;
    int timed = __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED);
    uint64_t begin = timed ? timer_get_ticks() : 0;
    int result = pt_map_anon_batch(child->pgdir, va, pages, nr, shared);
    if (timed)
        a20_perf_add(A20_PERF_MM_FORK_MAP_TICKS, timer_get_ticks() - begin);
    return result;
#else
    (void)child_ptr;
    (void)va;
    (void)pages;
    (void)nr;
    (void)shared;
    return -EOPNOTSUPP;
#endif
}

#if defined(ARCH_HAS_PGTABLE_OPS)
/* Called with the source node locked and parent->lock holding VMA policy
 * stable. A canonical cache-owned private leaf can be recovered through the
 * child's retained file mapping. A private copy made writable and later
 * mprotected read-only has no cache owner and must still be inherited.
 * Preserve executable and small mappings: their first child faults cost more
 * than cloning their PTEs on A20. Elision targets bulk file data (>= 1 MiB). */
static int cow_file_leaf_refaultable(const mm_seg_t *vma, pt_meta_t *meta,
                                     int index, int level, pte_t source)
{
    if (!vma || level != 0 || !meta || !vma->file || !vma->file_vnode ||
        vma->end - vma->start < 1024 * 1024 || (vma->vm_flags & VM_EXEC) ||
        (vma->vm_flags & (VM_FILE | VM_SHARED | VM_VMO | VM_PFNMAP)) != VM_FILE ||
        !(source & PTE_V) || !arch_pte_is_leaf(source) || (source & PTE_W) ||
        MM_ST_GET_CLASS(meta->cls[index]) != MM_ST_FILE_PRIVATE ||
        (meta->safe[index >> 3] & MM_SAFE_MASK) || !meta->cache_owner ||
        arch_fork_requires_private_copy())
        return 0;
    page_cache_page_t *owner = meta->cache_owner[index];
    return owner && page_cache_pfn(owner) == phys_to_pfn(arch_pte_addr(source));
}
/* Read-only proof, with parent->lock held: avoid constructing empty clone
 * transactions for recoverable cache leaves. A full-table positive proof
 * survives read-only observations; node writers and metadata mutators revoke
 * it before the next observation. VMA policy is checked each time. */
static int cow_file_range_refaultable(const mm_seg_t *vma, pte_t *node,
                                      int first, int count)
{
    if (!vma || !vma->file || !vma->file_vnode ||
        vma->end - vma->start < 1024 * 1024 || (vma->vm_flags & VM_EXEC) ||
        (vma->vm_flags & (VM_FILE | VM_SHARED | VM_VMO | VM_PFNMAP)) != VM_FILE ||
        arch_fork_requires_private_copy())
        return 0;
    pt_meta_t *meta = mm_pt_meta(node);
    if (!meta || !meta->cache_owner)
        return 0;
    int recoverable = 1;
    mm_pt_node_observe_lock(node);
    if (meta->fork_file_clean) {
        a20_perf_count(A20_PERF_MM_FORK_FILE_PROOF_HITS);
        mm_pt_node_unlock(node);
        return 1;
    }
    unsigned scanned = 0;
    for (int i = first; i < first + count; i++) {
        scanned++;
        pte_t source = node[i];
        if (!source)
            continue;
        page_cache_page_t *owner = meta->cache_owner[i];
        if (!(source & PTE_V) || !arch_pte_is_leaf(source) || (source & PTE_W) ||
            MM_ST_GET_CLASS(meta->cls[i]) != MM_ST_FILE_PRIVATE ||
            (meta->safe[i >> 3] & MM_SAFE_MASK) || !owner ||
            page_cache_pfn(owner) != phys_to_pfn(arch_pte_addr(source))) {
            recoverable = 0;
            break;
        }
    }
    a20_perf_add(A20_PERF_MM_FORK_FILE_PROOF_SCANS, scanned);
    if (recoverable && first == 0 && count == arch_pt_level_entries(0))
        meta->fork_file_clean = 1;
    mm_pt_node_unlock(node);
    return recoverable;
}

#endif

/* MM_FORK_SNAPSHOT_PIN: snapshot and pin the source leaf under the parent's
 * node lock.  The lock is RELEASED by a20_cow_share_unlock() on every success
 * path; returning 0 leaves no lock held.  Pinning and snapshotting are one step
 * because a racing COW can otherwise free the frame before it is retained.
 * The transaction that decides share-vs-copy, downgrades the parent and maps
 * the child only after this lock is dropped is kernel/mm/cow_clone.rs; the
 * atomic step itself has to stay here, next to the page-table walk.
 *
 * The node lock and the per-PTE status sidecar exist only on the
 * ARCH_HAS_PGTABLE_OPS backends.  arm32's short-descriptor page tables have
 * neither: there is no separate node lock to take (the whole clone runs under
 * parent->lock, see mm_fork()) and no status byte to keep in step. */
/* `src_pte` and `stab` can come either from a fresh root walk or from the
 * active C walker. In the latter case parent->lock keeps the borrowed table
 * alive. We still take the node lock and re-read the slot before pinning; the
 * borrowed address only avoids rediscovering the same leaf from the root. */
static int a20_cow_share_snapshot_from_slot(
    mm_struct_t *parent, uintptr_t va, int level, pte_t *src_pte, pte_t *stab,
    mm_seg_t *known_vma, struct a20_cow_shared_snapshot *out)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (stab)
        mm_pt_node_lock(stab);
#endif
    pte_t source = *src_pte;
    if (!(source & PTE_V) || !arch_pte_is_leaf(source)) {
#if defined(ARCH_HAS_PGTABLE_OPS)
        if (stab)
            mm_pt_node_unlock(stab);
#endif
        return 0;
    }
    mm_seg_t *vma = known_vma ? known_vma : mm_seg_find(parent, va);
    if (known_vma && (va < known_vma->start || va >= known_vma->end)) {
#if defined(ARCH_HAS_PGTABLE_OPS)
        if (stab)
            mm_pt_node_unlock(stab);
#endif
        return 0;
    }
    int is_vmo = vma && (vma->vm_flags & VM_VMO);
    int is_file = vma && (vma->vm_flags & VM_FILE);
    pfn_t pfn = phys_to_pfn(arch_pte_addr(source));
    uint8_t cls = mm_fork_page_class(stab, level, va, vma);
    page_cache_page_t *pcp = NULL;
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (stab && cow_file_leaf_refaultable(vma, mm_pt_meta(stab),
                                         arch_pt_vpn(va, level), level, source)) {
        mm_pt_node_unlock(stab);
        return 0;
    }
    if (is_file && stab) {
        pcp = mm_pt_cache_peek(stab, arch_pt_vpn(va, level));
        if (pcp && page_cache_pfn(pcp) == pfn)
            page_cache_pin(pcp);
        else
            pcp = NULL;
    }
#endif
    if (!pcp && is_file && cls != MM_ST_ANON_MAPPED)
        pcp = mm_file_cache_mapping_get(vma, va, pfn);
    if (!pcp && !is_vmo)
        frame_get(pfn);
    out->slot = src_pte;
    out->node = stab;
    out->pa = arch_pte_addr(source);
    out->pfn = pfn;
    out->raw_flags = arch_pte_flags(source);
    out->cow_flags = mm_cow_flags(source);
    out->w_or_cow = (source & (PTE_W | PTE_COW)) != 0;
    out->class = cls;
    out->pcp = pcp;
    out->kind = pcp ? 2 : (is_vmo ? 0 : 1);
    return 1;
}

int a20_cow_share_snapshot(void *parent_ptr, uintptr_t va, int level,
                           struct a20_cow_shared_snapshot *out)
{
    mm_struct_t *parent = parent_ptr;
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    out->pfn = PFN_NONE;
    pte_t *src_pte = pt_lookup_leaf(parent->pgdir, va, NULL, NULL, NULL);
    if (!src_pte)
        return 0;
    pte_t *stab = NULL;
#if defined(ARCH_HAS_PGTABLE_OPS)
    stab = mm_pt_leaf_table(parent->pgdir, va);
#endif
    return a20_cow_share_snapshot_from_slot(parent, va, level, src_pte, stab,
                                            NULL, out);
}

/* The walker owns parent->lock and lends this leaf location only for the
 * duration of the clone call. Validate its geometry without another root
 * descent, then let the shared snapshot take node exclusion, re-read the PTE,
 * and pin the backing reference before it returns. */
int a20_cow_share_snapshot_at_vma(void *parent_ptr, uintptr_t va, int level,
                                  pte_t *slot, pte_t *node, mm_seg_t *vma,
                                  struct a20_cow_shared_snapshot *out)
{
    mm_struct_t *parent = parent_ptr;
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    out->pfn = PFN_NONE;
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (level < 0 || level > ARCH_PT_ROOT_LEVEL)
        return 0;
    int entries = arch_pt_level_entries(level);
    int index = arch_pt_vpn(va, level);
    if (!slot || !node || entries <= 0 || index < 0 || index >= entries ||
        slot != &node[index])
        return 0;
    return a20_cow_share_snapshot_from_slot(parent, va, level, slot, node, vma,
                                            out);
#else
    (void)parent;
    (void)va;
    (void)level;
    (void)slot;
    (void)node;
    (void)vma;
    return 0;
#endif
}

int a20_cow_share_snapshot_at(void *parent_ptr, uintptr_t va, int level,
                              pte_t *slot, pte_t *node,
                              struct a20_cow_shared_snapshot *out)
{
    return a20_cow_share_snapshot_at_vma(parent_ptr, va, level, slot, node, NULL,
                                         out);
}

static void cow_share_downgrade(void *parent_ptr, uintptr_t va, int level,
                                const struct a20_cow_shared_snapshot *snapshot,
                                uintptr_t flags, pt_meta_t *meta)
{
    mm_struct_t *parent = parent_ptr;
    pte_t *src_pte = snapshot ? snapshot->slot : NULL;
    if (!src_pte)
        return;
    uintptr_t pa = snapshot->pa;
    pte_t desired;
#ifdef ARCH_HAS_PTE_BLOCK
    size_t leaf_size = vm_pt_level_size(level);
    desired = (leaf_size >= PMD_SIZE)
                   ? arch_pte_block((paddr_t)pa, (pte_t)flags)
                   : arch_pte_leaf((paddr_t)pa, (pte_t)flags);
#else
    desired = arch_pte_leaf((paddr_t)pa, (pte_t)flags);
#endif
    /* Repeated forks often see an already clean, read-only COW leaf.
     * Identical PTEs require neither a rewrite nor a TLB invalidation. */
    if (*src_pte != desired) {
        *src_pte = desired;
        mm_tlb_note_change(parent, va, vm_pt_level_size(level));
    }
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (snapshot->node) {
        int idx = arch_pt_vpn(va, level);
        if (meta)
            (void)mm_pt_sync_status_meta(meta, idx,
                                         (uint8_t)snapshot->class, *src_pte);
        else
            (void)mm_pt_sync_status(snapshot->node, level, idx,
                                    (uint8_t)snapshot->class);
    }
#else
    /* No status sidecar to bring in step: the PTE rewrite above is the whole
     * of the parent-side downgrade on the arm32 short-descriptor backend. */
    (void)level;
    (void)meta;
#endif
}

void a20_cow_share_downgrade(void *parent_ptr, uintptr_t va, int level,
                             const struct a20_cow_shared_snapshot *snapshot,
                             uintptr_t flags)
{
    cow_share_downgrade(parent_ptr, va, level, snapshot, flags, NULL);
}

void a20_cow_share_unlock(const struct a20_cow_shared_snapshot *snapshot)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (snapshot && snapshot->node)
        mm_pt_node_unlock(snapshot->node);
#else
    /* a20_cow_share_snapshot() took no lock on this backend, so there is
     * nothing to release; the Rust clone transaction still calls this on
     * every success path. */
    (void)snapshot;
#endif
}

#ifndef CONFIG_NOMMU
#if defined(ARCH_HAS_PGTABLE_OPS)
/* The child is unpublished and has no concurrent page-table writer.  Use the
 * absent-subtree span reported by one lookup when it covers the whole batch;
 * otherwise accept only an absent level-0 slot and inspect that stable table
 * with one non-allocating walk.  A returned leaf (including huge/swap) always
 * declines before taking the source node lock or changing parent references. */
static int cow_child_batch_empty(mm_struct_t *child, uintptr_t start,
                                 size_t nr, int first)
{
    if (!child || !child->pgdir || nr == 0 || first < 0)
        return 0;
    int entries = arch_pt_level_entries(0);
    if (entries <= 0 || nr > (size_t)entries || first >= entries ||
        nr > (size_t)(entries - first))
        return 0;
    uintptr_t bytes = nr * PAGE_SIZE;

    int level = -1;
    vaddr_t base = 0;
    size_t span = 0;
    pte_t *leaf = pt_lookup_leaf(child->pgdir, start, &level, &base, &span);
    if (leaf)
        return 0;
    if (level < 0 || level > ARCH_PT_ROOT_LEVEL || span == 0 || base > start)
        return 0;

    uintptr_t offset = start - base;
    if (offset <= span && bytes <= span - offset)
        return 1;

    if (level != 0 || base != start || span != PAGE_SIZE ||
        arch_pt_vpn(start, 0) != first)
        return 0;
    pte_t *first_slot = pt_walk(child->pgdir, start, 0);
    if (!first_slot)
        return 0;
    for (size_t i = 0; i < nr; i++) {
        if (first_slot[i] != 0)
            return 0;
    }
    return 1;
}
#endif /* ARCH_HAS_PGTABLE_OPS */

/* The caller is the active level-0 C walker under parent->lock. `node` is its
 * borrowed leaf table, whose lifetime is protected until this call returns;
 * this helper holds only this one source-node lock at a time. Preflight and pin
 * one possibly sparse 4K run, downgrading parents only for private mappings.
 * Zero is side-effect-free fallback. */
int a20_cow_share_anon_batch_snapshot(
    void *parent_ptr, void *child_ptr, uintptr_t start, pte_t *node,
    struct a20_cow_anon_batch_page *out, size_t nr, int shared)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    mm_struct_t *parent = parent_ptr;
    mm_struct_t *child = child_ptr;
    if (!parent || !child || !parent->pgdir || !child->pgdir || !node || !out ||
        nr == 0 || nr > A20_COW_ANON_BATCH_PAGES ||
        start > (uintptr_t)-1 - nr * PAGE_SIZE)
        return 0;
    const uintptr_t bytes = nr * PAGE_SIZE;
    if (start & (PAGE_SIZE - 1))
        return 0;

    int first = arch_pt_vpn(start, 0);
    int entries = arch_pt_level_entries(0);
    if (first < 0 || entries <= 0 || nr > (size_t)(entries - first))
        return 0;
    pt_meta_t *node_meta = mm_pt_meta(node);
    if (!node_meta || mm_pt_meta_level(node_meta) != 0)
        return 0;

    uintptr_t end = start + bytes;
    /* mm_fork() holds parent->lock, so this single VMA record and its policy
     * remain stable for the complete source snapshot. */
    mm_seg_t *vma = mm_seg_find(parent, start);
    int borrowed_vmo = vma && (vma->vm_flags & VM_VMO) && shared;
    const uint64_t excluded = VM_PFNMAP | VM_DONTFORK | VM_WIPEONFORK;
    if (vma) {
        if (start < vma->start || end > vma->end ||
            (!(vma->vm_flags & (VM_ANON | VM_FILE)) && !borrowed_vmo) ||
            ((vma->vm_flags & VM_VMO) && !borrowed_vmo) ||
            (vma->vm_flags & excluded))
            return 0;
    } else {
        /* Legacy brk/stack pages have no VMA. Prove the entire batch lies in
         * a gap, not merely its endpoints; a protected VMA may sit inside. */
        for (mm_seg_t *v = parent->mmap; v; v = v->next) {
            if (v->start >= end)
                break;
            if (v->end > start)
                return 0;
        }
    }

    /* Inspect the unpublished destination before source pinning/downgrade.
     * An absent upper subtree costs one lookup; an existing empty L0 table
     * costs one lookup plus one walk instead of per-page root walks. */
    if (!cow_child_batch_empty(child, start, nr, first))
        return 0;

    pte_t *slots[A20_COW_ANON_BATCH_PAGES];
    pfn_t pfns[A20_COW_ANON_BATCH_PAGES];
    pfn_t frame_pfns[A20_COW_ANON_BATCH_PAGES];
    size_t frame_count = 0;
    mm_pt_node_lock(node);
    for (size_t i = 0; i < nr; i++) {
        int index = first + (int)i;
        pte_t *slot = &node[index];
        pte_t source = *slot;
        slots[i] = slot;
        pfns[i] = 0;
        out[i] = (struct a20_cow_anon_batch_page){0};
        if (!source)
            continue;
        uint8_t status = node_meta->cls[index];
        uint32_t cls = MM_ST_GET_CLASS(status);
        if (!(source & PTE_V) || !arch_pte_is_leaf(source) ||
            (!vma && cls != MM_ST_ANON_MAPPED) ||
            (cls != MM_ST_ANON_MAPPED && cls != MM_ST_ANON_SHARED &&
             cls != MM_ST_FILE_PRIVATE && cls != MM_ST_FILE_SHARED &&
             !(borrowed_vmo && cls == MM_ST_VMO)) ||
            (borrowed_vmo && cls != MM_ST_VMO) ||
            (node_meta->safe[index >> 3] &
             (MM_SAFE_UFFD | MM_SAFE_NO_FA))) {
            mm_pt_node_unlock(node);
            return 0;
        }
        pfn_t pfn = phys_to_pfn(arch_pte_addr(source));
        if (!pfn_valid(pfn)) {
            mm_pt_node_unlock(node);
            return 0;
        }
        if (!shared && cow_file_leaf_refaultable(vma, node_meta, index, 0, source))
            continue;
        pfns[i] = pfn;
        out[i].pfn = pfn;
        out[i].class = cls;
        out[i].flags = shared ? arch_pte_flags(source) : mm_cow_flags(source);
    }

    /* All entries are validated before any reference or PTE changes. */
    for (size_t i = 0; i < nr; i++) {
        if (!out[i].class)
            continue;
        /* The child already retained the VMO through its VMA. Its leaves
         * borrow canonical frames, just like scalar VMO cloning; no PTE-owned
         * frame reference is acquired or released on rollback. */
        if (borrowed_vmo)
            continue;
        uintptr_t va = start + (uintptr_t)i * PAGE_SIZE;
        page_cache_page_t *pcp = vma && (vma->vm_flags & VM_FILE) &&
                                         node_meta->cache_owner
            ? node_meta->cache_owner[first + (int)i] : NULL;
        if (pcp && page_cache_pfn(pcp) == pfns[i])
            page_cache_pin(pcp);
        else
            pcp = NULL;
        if (!pcp && vma && (vma->vm_flags & VM_FILE) &&
            out[i].class != MM_ST_ANON_MAPPED)
            pcp = mm_file_cache_mapping_get(vma, va, pfns[i]);
        if (pcp) {
            out[i].pcp = pcp;
            out[i].kind = 2;
        } else {
            out[i].kind = 1;
            frame_pfns[frame_count++] = pfns[i];
        }
    }
    frame_get_many(frame_pfns, frame_count);
    for (size_t i = 0; i < nr; i++) {
        if (!out[i].class)
            continue;
        pte_t source = *slots[i];
        if (!shared && (source & (PTE_W | PTE_COW))) {
            struct a20_cow_shared_snapshot snap = {0};
            uintptr_t va = start + (uintptr_t)i * PAGE_SIZE;
            snap.slot = slots[i];
            snap.node = node;
            snap.pa = arch_pte_addr(source);
            snap.pfn = pfns[i];
            snap.raw_flags = arch_pte_flags(source);
            snap.cow_flags = mm_cow_flags(source);
            snap.w_or_cow = 1;
            snap.class = out[i].class;
            cow_share_downgrade(parent, va, 0, &snap, snap.cow_flags,
                                node_meta);
        }
    }
    mm_pt_node_unlock(node);
    return 1;
#else
    (void)parent_ptr;
    (void)child_ptr;
    (void)start;
    (void)node;
    (void)out;
    (void)nr;
    (void)shared;
    return 0;
#endif /* ARCH_HAS_PGTABLE_OPS */
}
#endif /* !CONFIG_NOMMU */

int a20_cow_requires_private_copy(void)
{
    return arch_fork_requires_private_copy();
}

void a20_cow_rss_add(void *child, uintptr_t pages)
{
    mm_rss_add(child, pages);
}

void a20_cow_release_ref(const struct a20_cow_shared_snapshot *src)
{
    if (!src)
        return;
    if (src->kind == 2 && src->pcp)
        page_cache_put(src->pcp);
    else if (src->kind == 1)
        frame_put(src->pfn);
}

int mm_fork_clone_page(mm_struct_t *child, mm_struct_t *parent, vaddr_t va,
                       int shared) {
    return a20_fork_clone_page_rust(child, parent, va, shared);
}

static int mm_fork_clone_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared,
                                int checked, mm_seg_t *vma);

int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                        vaddr_t start, vaddr_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    /* brk/stack may contain enormous untouched holes, and can overlap the
     * VMA clone above. Visit only existing leaves, but keep the per-address
     * policy/base/destination checks rather than cloning them a second time.
     * The caller holds parent->lock, as for the existing VMA walker. */
    return mm_fork_clone_level(child, parent, parent->pgdir, ARCH_PT_ROOT_LEVEL,
                              0, start, end, shared, 1, NULL);
}

int mm_fork_clone_leaf(mm_struct_t *child, mm_struct_t *parent,
                       pte_t *src_pte, vaddr_t va, int level,
                       int shared) {
    if (!src_pte || !(*src_pte & PTE_V) || !arch_pte_is_leaf(*src_pte))
        return 0;
    return a20_cow_clone_leaf_rust(child, parent, va, level, shared);
}

static int mm_fork_clone_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared,
                                int checked, mm_seg_t *vma) {
    if (!table || start >= end)
        return 0;

    size_t span = vm_pt_level_size(level);
    int entries = arch_pt_level_entries(level);
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    if (!checked && !shared && level == 0 && vma &&
        start >= vma->start && end <= vma->end) {
        int first = start > base ? (start - base) / PAGE_SIZE : 0;
        int last = end - base < (vaddr_t)entries * PAGE_SIZE
            ? (int)((end - base) / PAGE_SIZE) : entries;
        if (first < last && cow_file_range_refaultable(vma, table, first, last - first))
            return 0;
    }
#endif
    /* A VMA may begin near the end of every table on its path. Jump to its
     * first overlapping slot instead of scanning the preceding entries. Keep
     * the wide offset until the bound check (public callers can pass disjoint
     * ranges); partial huge leaves still follow the checked base rule below. */
    vaddr_t first_slot = start > base ? (start - base) / span : 0;
    if (first_slot >= (vaddr_t)entries)
        return 0;
    for (int i = (int)first_slot; i < entries; i++) {
        vaddr_t entry_base = base + (vaddr_t)i * span;
        vaddr_t entry_end = entry_base + span;
        if (entry_end <= start)
            continue;
        if (entry_base >= end)
            break;

        pte_t *pte = &table[i];
        /* Empty slots carry no references. In particular, do not construct
         * a Rust batch, probe the child, or lock the source for a hole-only
         * prefix of a sparsely populated leaf table. */
        if (!*pte)
            continue;

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
        if (level == 0 && entry_base >= start &&
            ((!checked && vma && entry_base >= vma->start && entry_base < vma->end) ||
             (checked && !vma && !mm_seg_find(parent, entry_base)))) {
            size_t nr = A20_COW_ANON_BATCH_PAGES;
            size_t range_pages = (end - entry_base) / PAGE_SIZE;
            size_t vma_pages = vma ? (vma->end - entry_base) / PAGE_SIZE : range_pages;
            if (nr > (size_t)(entries - i))
                nr = entries - i;
            if (nr > range_pages)
                nr = range_pages;
            if (nr > vma_pages)
                nr = vma_pages;
            if (nr >= 2) {
                int timed = __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED);
                uint64_t begin = timed ? timer_get_ticks() : 0;
                int batch = a20_cow_clone_anon_batch_rust(
                    child, parent, entry_base, table, nr, shared);
                if (timed) {
                    a20_perf_count(A20_PERF_MM_FORK_BATCH_CALLS);
                    a20_perf_add(A20_PERF_MM_FORK_BATCH_TICKS, timer_get_ticks() - begin);
                }
                if (batch < 0)
                    return batch;
                if (batch > 0) {
                    i += (int)nr - 1;
                    continue;
                }
            }
        }
#endif

#ifdef CONFIG_SWAP
        if (level == 0 && pte_is_swap(*pte)) {
            int r = checked
                        ? mm_fork_clone_page(child, parent, entry_base, shared)
                        : mm_clone_swap_page(child, entry_base, parent, entry_base);
            if (r < 0 && (checked || (r != -EEXIST && r != -EAGAIN)))
                return r;
            continue;
        }
#endif
        if (!(*pte & PTE_V))
            continue;

        if (arch_pte_is_leaf(*pte)) {
            /* The old address loop only cloned a huge leaf at its base;
             * starting in its middle must not acquire an extra reference. */
            if (checked && entry_base < start)
                continue;
            int r;
            int timed = __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED);
            uint64_t begin = timed ? timer_get_ticks() : 0;
#if defined(ARCH_HAS_PGTABLE_OPS)
            /* This leaf and its table came from the active walker. The caller
             * holds parent->lock, so Rust can apply checked policy/dst rules
             * and the C snapshot can validate/pin this exact slot without a
             * second root walk. */
            r = a20_cow_clone_leaf_at_rust(child, parent, entry_base, level,
                                           shared, checked, pte, table, vma);
#else
            r = checked
                    ? mm_fork_clone_page(child, parent, entry_base, shared)
                    : mm_fork_clone_leaf(child, parent, pte, entry_base, level,
                                         shared);
#endif
            if (timed) {
                a20_perf_count(A20_PERF_MM_FORK_SCALAR_CALLS);
                a20_perf_add(A20_PERF_MM_FORK_SCALAR_TICKS, timer_get_ticks() - begin);
            }
            if (r < 0)
                return r;
            continue;
        }

        if (level > 0) {
            int r = mm_fork_clone_level(child, parent, arch_pte_to_ptr(*pte),
                                       level - 1, entry_base, start, end, shared,
                                       checked, vma);
            if (r < 0)
                return r;
        }
    }
    return 0;
}

int mm_fork_clone_present_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared) {
    return mm_fork_clone_level(child, parent, table, level, base, start, end,
                              shared, 0, NULL);
}

int mm_fork_clone_present_range_vma(mm_struct_t *child, mm_struct_t *parent,
                                    mm_seg_t *vma, int shared) {
    vaddr_t start = ROUND_DOWN(vma->start, PAGE_SIZE);
    vaddr_t end = ROUND_UP(vma->end, PAGE_SIZE);
    return mm_fork_clone_level(child, parent, parent->pgdir, ARCH_PT_ROOT_LEVEL,
                               0, start, end, shared, 0, vma);
}

int mm_fork_clone_present_range(mm_struct_t *child, mm_struct_t *parent,
                                vaddr_t start, vaddr_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    return mm_fork_clone_present_level(child, parent, parent->pgdir, ARCH_PT_ROOT_LEVEL,
                                       0, start, end, shared);
}

#else /* CONFIG_NOMMU */

/* Rust keeps one cross-architecture clone entry point. NOMMU has no leaves to
 * batch, so its C snapshot hook is an unconditional no-op fallback. */
int a20_cow_share_anon_batch_snapshot(void *parent, void *child,
                                      uintptr_t start, pte_t *node,
                                      struct a20_cow_anon_batch_page *out,
                                      size_t nr, int shared)
{
    (void)parent; (void)child; (void)start; (void)node; (void)out; (void)nr;
    (void)shared;
    return 0;
}

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

int a20_cow_map_child_anon_batch(
    void *child, uintptr_t va,
    const struct a20_cow_anon_batch_page *pages, size_t nr, int shared)
{
    (void)child;
    (void)va;
    (void)pages;
    (void)nr;
    (void)shared;
    return -EOPNOTSUPP;
}

int mm_fork_clone_present_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared)
{ (void)child; (void)parent; (void)table; (void)level; (void)base;
  (void)start; (void)end; (void)shared; return 0; }

int mm_fork_clone_present_range(mm_struct_t *child, mm_struct_t *parent,
                                vaddr_t start, vaddr_t end, int shared)
{ (void)child; (void)parent; (void)start; (void)end; (void)shared; return 0; }

int mm_fork_clone_present_range_vma(mm_struct_t *child, mm_struct_t *parent,
                                    mm_seg_t *vma, int shared)
{ (void)child; (void)parent; (void)vma; (void)shared; return 0; }

#endif /* CONFIG_NOMMU */
