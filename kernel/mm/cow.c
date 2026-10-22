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
                      uintptr_t flags, uint32_t class, int level)
{
    mm_struct_t *child = child_ptr;
    paddr_t pa = pfn_to_phys(pfn);
    if (level > 0)
        return pt_map_huge(child, va, pa, (pte_t)flags, (uint8_t)class);
    return pt_map(child->pgdir, va, pa, (pte_t)flags);
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
    page_cache_page_t *pcp =
        is_vmo ? NULL : mm_file_cache_mapping_get(vma, va, pfn);
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
            int r = checked
                        ? mm_fork_clone_page(child, parent, entry_base, shared)
                        : mm_fork_clone_leaf(child, parent, pte, entry_base, level,
                                             shared);
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
