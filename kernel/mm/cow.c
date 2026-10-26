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
                                      int checked, pte_t *slot, pte_t *node);
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
extern int a20_cow_clone_anon_batch_rust(mm_struct_t *child,
                                         mm_struct_t *parent, vaddr_t va,
                                         pte_t *node, int shared);
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
    return pt_map_anon_batch(child->pgdir, va, pages, nr, shared);
#else
    (void)child_ptr;
    (void)va;
    (void)pages;
    (void)nr;
    return -EOPNOTSUPP;
#endif
}

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
    struct a20_cow_shared_snapshot *out)
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
    mm_seg_t *vma = mm_seg_find(parent, va);
    int is_vmo = vma && (vma->vm_flags & VM_VMO);
    pfn_t pfn = phys_to_pfn(arch_pte_addr(source));
    uint8_t cls = mm_fork_page_class(stab, level, va, vma);
    page_cache_page_t *pcp = NULL;
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (!is_vmo && stab) {
        pcp = mm_pt_cache_peek(stab, arch_pt_vpn(va, level));
        if (pcp && page_cache_pfn(pcp) == pfn)
            page_cache_pin(pcp);
        else
            pcp = NULL;
    }
#endif
    if (!pcp && !is_vmo)
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
                                            out);
}

/* The walker owns parent->lock and lends this leaf location only for the
 * duration of the clone call. Validate its geometry without another root
 * descent, then let the shared snapshot take node exclusion, re-read the PTE,
 * and pin the backing reference before it returns. */
int a20_cow_share_snapshot_at(void *parent_ptr, uintptr_t va, int level,
                              pte_t *slot, pte_t *node,
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
    return a20_cow_share_snapshot_from_slot(parent, va, level, slot, node,
                                            out);
#else
    (void)parent;
    (void)va;
    (void)level;
    (void)slot;
    (void)node;
    return 0;
#endif
}

void a20_cow_share_downgrade(void *parent_ptr, uintptr_t va, int level,
                             const struct a20_cow_shared_snapshot *snapshot,
                             uintptr_t flags)
{
    mm_struct_t *parent = parent_ptr;
    pte_t *src_pte = snapshot ? snapshot->slot : NULL;
    if (!src_pte)
        return;
    uintptr_t pa = snapshot->pa;
#ifdef ARCH_HAS_PTE_BLOCK
    size_t leaf_size = vm_pt_level_size(level);
    *src_pte = (leaf_size >= PMD_SIZE)
                   ? arch_pte_block((paddr_t)pa, (pte_t)flags)
                   : arch_pte_leaf((paddr_t)pa, (pte_t)flags);
#else
    *src_pte = arch_pte_leaf((paddr_t)pa, (pte_t)flags);
#endif
    mm_tlb_note_change(parent, va, vm_pt_level_size(level));
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (snapshot->node)
        (void)mm_pt_sync_status(snapshot->node, level, arch_pt_vpn(va, level),
                                (uint8_t)snapshot->class);
#else
    /* No status sidecar to bring in step: the PTE rewrite above is the whole
     * of the parent-side downgrade on the arm32 short-descriptor backend. */
    (void)level;
#endif
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
                                 uintptr_t bytes, int first)
{
    if (!child || !child->pgdir || bytes == 0 || first < 0)
        return 0;
    int entries = arch_pt_level_entries(0);
    if (entries <= 0 || first >= entries ||
        first > entries - A20_COW_ANON_BATCH_PAGES)
        return 0;

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
    for (unsigned i = 0; i < A20_COW_ANON_BATCH_PAGES; i++) {
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
    struct a20_cow_anon_batch_page *out, int shared)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    mm_struct_t *parent = parent_ptr;
    mm_struct_t *child = child_ptr;
    const uintptr_t bytes = A20_COW_ANON_BATCH_PAGES * PAGE_SIZE;
    if (!parent || !child || !parent->pgdir || !child->pgdir || !node || !out)
        return 0;
    if ((start & (PAGE_SIZE - 1)) || start > (uintptr_t)-1 - bytes)
        return 0;

    int first = arch_pt_vpn(start, 0);
    int entries = arch_pt_level_entries(0);
    if (first < 0 || entries <= 0)
        return 0;
    if (first % A20_COW_ANON_BATCH_PAGES != 0 ||
        first > entries - A20_COW_ANON_BATCH_PAGES)
        return 0;
    pt_meta_t *node_meta = mm_pt_meta(node);
    if (!node_meta || mm_pt_meta_level(node_meta) != 0)
        return 0;

    uintptr_t end = start + bytes;
    /* mm_fork() holds parent->lock, so this single VMA record and its policy
     * remain stable for the complete source snapshot. */
    mm_seg_t *vma = mm_seg_find(parent, start);
    const uint64_t excluded = VM_PFNMAP | VM_VMO |
                              VM_DONTFORK | VM_WIPEONFORK;
    if (!vma || start < vma->start || end > vma->end)
        return 0;
    if (!(vma->vm_flags & (VM_ANON | VM_FILE)) ||
        (vma->vm_flags & excluded))
        return 0;

    /* Inspect the unpublished destination before source pinning/downgrade.
     * An absent upper subtree costs one lookup; an existing empty L0 table
     * costs one lookup plus one walk instead of 64 repeated root walks. */
    if (!cow_child_batch_empty(child, start, bytes, first))
        return 0;

    pte_t *slots[A20_COW_ANON_BATCH_PAGES];
    pfn_t pfns[A20_COW_ANON_BATCH_PAGES];
    pfn_t frame_pfns[A20_COW_ANON_BATCH_PAGES];
    size_t frame_count = 0;
    mm_pt_node_lock(node);
    for (unsigned i = 0; i < A20_COW_ANON_BATCH_PAGES; i++) {
        int index = first + (int)i;
        pte_t *slot = &node[index];
        pte_t source = *slot;
        slots[i] = slot;
        pfns[i] = 0;
        out[i] = (struct a20_cow_anon_batch_page){0};
        if (!source)
            continue;
        uint8_t status = mm_pt_peek(node, 0, index);
        uint32_t cls = MM_ST_GET_CLASS(status);
        if (!(source & PTE_V) || !arch_pte_is_leaf(source) ||
            (cls != MM_ST_ANON_MAPPED && cls != MM_ST_ANON_SHARED &&
             cls != MM_ST_FILE_PRIVATE && cls != MM_ST_FILE_SHARED) ||
            mm_pt_safe_test(node, 0, index, MM_SAFE_UFFD) ||
            mm_pt_safe_test(node, 0, index, MM_SAFE_NO_FA)) {
            mm_pt_node_unlock(node);
            return 0;
        }
        pfn_t pfn = phys_to_pfn(arch_pte_addr(source));
        if (!pfn_valid(pfn)) {
            mm_pt_node_unlock(node);
            return 0;
        }
        pfns[i] = pfn;
        out[i].pfn = pfn;
        out[i].class = cls;
        out[i].flags = shared ? arch_pte_flags(source) : mm_cow_flags(source);
    }

    /* All entries are validated before any reference or PTE changes. */
    for (unsigned i = 0; i < A20_COW_ANON_BATCH_PAGES; i++) {
        if (!out[i].class)
            continue;
        uintptr_t va = start + (uintptr_t)i * PAGE_SIZE;
        page_cache_page_t *pcp =
            mm_pt_cache_peek(node, first + (int)i);
        if (pcp && page_cache_pfn(pcp) == pfns[i])
            page_cache_pin(pcp);
        else
            pcp = NULL;
        if (!pcp)
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
    for (unsigned i = 0; i < A20_COW_ANON_BATCH_PAGES; i++) {
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
            a20_cow_share_downgrade(parent, va, 0, &snap, snap.cow_flags);
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
                                int checked);

int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                        vaddr_t start, vaddr_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    /* brk/stack may contain enormous untouched holes, and can overlap the
     * VMA clone above. Visit only existing leaves, but keep the per-address
     * policy/base/destination checks rather than cloning them a second time.
     * The caller holds parent->lock, as for the existing VMA walker. */
    return mm_fork_clone_level(child, parent, parent->pgdir, ARCH_PT_ROOT_LEVEL,
                              0, start, end, shared, 1);
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
                                int checked) {
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

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
        /* Try only a complete aligned run contained in the requested range.
         * The C snapshot does the all-or-nothing preflight; zero keeps the
         * existing scalar path untouched, and success skips exactly 64 leaves. */
        if (level == 0 && i % A20_COW_ANON_BATCH_PAGES == 0 &&
            entries - i >= A20_COW_ANON_BATCH_PAGES && entry_base >= start &&
            entry_base <= (vaddr_t)-1 - A20_COW_ANON_BATCH_PAGES * PAGE_SIZE) {
            vaddr_t batch_end =
                entry_base + A20_COW_ANON_BATCH_PAGES * PAGE_SIZE;
            if (batch_end <= end) {
                int batch = a20_cow_clone_anon_batch_rust(
                    child, parent, entry_base, table, shared);
                if (batch < 0)
                    return batch;
                if (batch > 0) {
                    i += A20_COW_ANON_BATCH_PAGES - 1;
                    continue;
                }
            }
        }
#endif

        pte_t *pte = &table[i];
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
#if defined(ARCH_HAS_PGTABLE_OPS)
            /* This leaf and its table came from the active walker. The caller
             * holds parent->lock, so Rust can apply checked policy/dst rules
             * and the C snapshot can validate/pin this exact slot without a
             * second root walk. */
            r = a20_cow_clone_leaf_at_rust(child, parent, entry_base, level,
                                           shared, checked, pte, table);
#else
            r = checked
                    ? mm_fork_clone_page(child, parent, entry_base, shared)
                    : mm_fork_clone_leaf(child, parent, pte, entry_base, level,
                                         shared);
#endif
            if (r < 0)
                return r;
            continue;
        }

        if (level > 0) {
            int r = mm_fork_clone_level(child, parent, arch_pte_to_ptr(*pte),
                                       level - 1, entry_base, start, end, shared,
                                       checked);
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
                              shared, 0);
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
                                      int shared)
{
    (void)parent; (void)child; (void)start; (void)node; (void)out; (void)shared;
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

#endif /* CONFIG_NOMMU */
