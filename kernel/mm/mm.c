/*
 * A20OS core MM — page-table primitives and the physical frame interface.
 *
 * The layer split is deliberate: this file owns the raw page-table walk
 * (pt_create/pt_walk/pt_map/pt_unmap/pt_clone*) and the mm_struct lifecycle,
 * while the per-architecture encoding lives behind arch_pte_* in
 * kernel/arch/<arch>/include/page_table.h.  The VMA list is in mm/vma.c and the
 * address-space/ASID/TLB machinery is in mm/vm.c, so the map/unmap/protect hot
 * paths stay readable on their own.
 *
 * Every function here takes a pt_root_t, not an mm_struct_t, so it is usable
 * before an mm exists (kernel bring-up) and by exec/fork on a half-built
 * address space.  Callers that mutate page tables must hold the owning
 * mm->lock; pt_walk() documents its own aliasing rules.
 */
#include "core/defs.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/pt.h"
#include "fs/page_cache.h"
#include "mm/failinject.h"
#include "mm/fault.h"
#include "mm/usercopy.h"
#include "mm/shutdown_write.h"
#include "core/panic.h"
#include "core/preempt.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/klog.h"
#include "proc/proc.h"
#include "mm/swap.h"

/* Forward declaration for pt_table_empty(), whose definition sits below inside
 * the page-table region with the rest of the walk.  The declaration has to
 * travel with it: left outside, a NOMMU build has a static declaration and no
 * definition, which -Werror calls an unused function. */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
static int pt_table_empty(pte_t *table, int level, int cleared_idx);
#endif

/* Huge-leaf installs through pt_map_huge (THP fault, fork clone, mremap
 * move).  A plain global rather than an a20_perf counter, for the same
 * reason mm_seg_dispatch_seg is: the audit-time [MM-ASM] reader must see it
 * even though a20_perf collection stays dormant until a /proc read arms it,
 * and even though the process whose page tables held the huge leaf has long
 * exited by the time the shutdown audit runs. */
uint64_t mm_huge_install_count;

static inline int pte_user_readable(pte_t pte) {
    return arch_pte_is_leaf(pte) && (pte & PTE_U) && (pte & PTE_R);
}

static inline int pte_user_writable(pte_t pte) {
    return arch_pte_is_leaf(pte) && (pte & PTE_U) && (pte & PTE_W);
}

static inline size_t pt_level_size(int level) {
    return PAGE_SIZE << (ARCH_PT_BITS * level);
}

/* The architecture may reserve a privileged prefix that must stay outside the
 * general frame allocator. The default limit is the end of the kernel image. */
__attribute__((weak)) paddr_t arch_pfa_init_limit(void)
{
    extern char _bss_end[];
    return va_to_pa(_bss_end);
}

void mm_init(void) {
    printf("[MM] mm_init begin\n");
    pfa_init(arch_pfa_init_limit());
    printf("[MM] pfa_init done\n");
    slab_init();
    printf("[MM] slab_init done\n");
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    mm_pt_core_init(); // 页表页 MCS 节点池（单级模型）
    printf("[MM] pt core init done\n");
#endif
    printf("[MM] Buddy+Slab: %d frames, %d free (%d MB)\n",
           (int)pfa.total_frames, (int)pfa.free_frames,
           (int)(pfa.free_frames * PAGE_SIZE / 1024 / 1024));
}

void *frame_alloc(void) {
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) return NULL;
    void *p = pfn_to_virt(pfn);
    memset(p, 0, PAGE_SIZE);
    return p;
}

/* Allocate without the reclaim hook: pfa_alloc_flags(0, can_reclaim = 0).
 *
 * For callers that hold a spinlock.  The reclaiming allocator reaches
 * oom_try_reclaim(), which swaps pages out and calls proc_force_exit(), and
 * tearing a victim down runs pt_unmap_leaf(), which takes a page-table node
 * MCS lock -- and mm->lock's holders reach this too, where reclaiming means
 * sleeping with interrupts off.
 *
 * Exhaustion is reported as NULL, so a caller must have a fallback.  Callers
 * that cannot fail this way are the ones whose failure is already a supported
 * outcome; see mm_pt_provision_anon(), whose result is (void) and documented as
 * best effort. */
void *frame_alloc_nr(void) {
    pfn_t pfn = pfa_alloc_flags(0, 0);
    if (pfn == PFN_NONE) return NULL;
    void *p = pfn_to_virt(pfn);
    memset(p, 0, PAGE_SIZE);
    return p;
}

/* Allocate a physical frame without zeroing it, for callers that overwrite
 * the contents immediately. */
void *frame_alloc_nz(void) {
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) return NULL;
    return pfn_to_virt(pfn);
}

void frame_free(void *addr) {
    if (!addr) return;
    pfn_t pfn = virt_to_pfn(addr);
    if (pfn_valid(pfn))
        pfa_free_page(pfn);
}

size_t frame_free_count(void) {
    return pfa_free_count();
}

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)

pte_t *pt_create(void) {
    pfn_t pfn = pfa_alloc(ARCH_PT_ROOT_ORDER);
    if (pfn == PFN_NONE)
        return NULL;
    pte_t *root = (pte_t *)pfn_to_virt(pfn);
    memset(root, 0, PAGE_SIZE << ARCH_PT_ROOT_ORDER);
    /* Give the root a descriptor so a cursor always has a node lock to take. */
    mm_pt_node_init(root, ARCH_PT_ROOT_LEVEL);
    return root;
}

static void pt_free_table(pte_t *table, int level) {
    if (!table)
        return;
    pfn_t pfn = virt_to_pfn(table);
    if (!pfn_valid(pfn))
        return;
    mm_pt_node_fini(table);
    pfa_free(pfn, level == ARCH_PT_ROOT_LEVEL ? ARCH_PT_ROOT_ORDER : 0);
}

/*
 * Recursively free a page-table level and its children.
 *
 * The leaf/table discrimination is the load-bearing part: arch_pte_is_leaf()
 * separates a mapping of data from a pointer to the next level, and freeing a
 * leaf as if it were a table would release a live data frame.  A leaf is
 * therefore skipped entirely -- its frame belongs to whatever the VMA backing
 * store owns, and pt_destroy_user() relies on that.
 *
 * A non-leaf PTE whose child frame is invalid (misaligned, or outside the
 * buddy-managed range) cannot be trusted, so the entry is zeroed and the
 * subtree is abandoned rather than dereferenced.  That turns a corrupt table
 * into a leak instead of a wild free.
 */
static void pt_destroy_level(pte_t *table, int level) {
    if (!table) return;
    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        uint64_t pte = table[i];
        if ((pte & PTE_V) && !arch_pte_is_leaf(pte)) {
            paddr_t next_pa = arch_pte_addr(pte);
            pfn_t next_pfn = phys_to_pfn(next_pa);
            if ((next_pa & (PAGE_SIZE - 1)) || !pfn_valid(next_pfn)) {
                kerr("pt_destroy: skip invalid non-leaf pte[%d]=0x%lx pa=0x%lx\n",
                     i, (unsigned long)pte, (unsigned long)next_pa);
                table[i] = 0;
                continue;
            }
            pte_t *next = arch_pte_to_ptr(pte);
            pt_destroy_level(next, level - 1);
            table[i] = 0;
        }
    }
    pt_free_table(table, level);
}

void pt_destroy(pt_root_t *pgdir) {
    pt_destroy_level(pgdir, ARCH_PT_ROOT_LEVEL);
}

/*
 * PT_WALK_CONTRACT:
 * - Returns an interior pointer INTO the page table, not a copy.  The caller
 *   must keep the address space pinned (mm->lock, or an mm reference) for as
 *   long as it holds the PTE, or a concurrent teardown can free the table.
 * - alloc == 0 never allocates.  A missing intermediate table returns NULL, so
 *   it is safe for a caller that holds only a read reference.
 * - A leaf found at an intermediate level means va is covered by a huge page
 *   larger than one level, which pt_walk cannot express; it returns NULL and
 *   the caller must fall back to pt_lookup_leaf().  Silently returning the
 *   huge-page PTE here would let a caller write a level-0 PTE into it.
 * - Under CONFIG_SWAP a swap entry at an intermediate level is returned as-is
 *   rather than treated as absent, so the caller can observe the swap PTE
 *   instead of allocating over the mapping.
 * - Returns NULL on allocation failure when alloc != 0.
 */
/* Return the table that owns the returned slot along with it. The owner is
 * borrowed for exactly the PTE's lifetime under the caller's existing MM
 * locking contract; this does not acquire or extend any page-table lock. */
static pte_t *pt_walk_with_owner(pt_root_t *pgdir, vaddr_t va, int alloc,
                                 pte_t **owner_out) {
    if (owner_out)
        *owner_out = NULL;
    pte_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 0; level--) {
        int vpn = arch_pt_vpn(va, level);
        pte_t pte = table[vpn];
#ifdef CONFIG_SWAP
        if (pte_is_swap(pte)) {
            if (owner_out)
                *owner_out = table;
            return &table[vpn];
        }
#endif
        if (pte & PTE_V) {
            if (arch_pte_is_leaf(pte))
                return NULL;
            table = arch_pte_to_ptr(pte);
        } else {
            if (!alloc) return NULL;
            /* MM-04 stage 1: fail the intermediate-node frame allocation the
             * way pfa_alloc_page() would, before the parent entry is linked. */
            if (mm_fail_inject_scoped(MM_FAIL_STAGE_PT_NODE))
                return NULL;
            pte_t *next = (pte_t *)frame_alloc();
            if (!next) return NULL;
            /* A new intermediate node needs its descriptor before the parent
             * entry becomes reachable, so a cursor that descends into it
             * always finds a lock. */
            mm_pt_node_init(next, level - 1);
            table[vpn] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
            mm_pt_note_present(table, level, vpn,
                               MM_ST_CLS_BYTE(MM_ST_PT_NODE));
            table = next;
        }
    }
    int vpn = arch_pt_vpn(va, 0);
    if (owner_out)
        *owner_out = table;
    return &table[vpn];
}

pte_t *pt_walk(pt_root_t *pgdir, vaddr_t va, int alloc) {
    return pt_walk_with_owner(pgdir, va, alloc, NULL);
}

pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t va, int *level_out,
                      vaddr_t *base_out, size_t *size_out) {
    if (!pgdir) return NULL;
    pte_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
        int idx = arch_pt_vpn(va, level);
        pte_t *pte = &table[idx];
#ifdef CONFIG_SWAP
        if (pte_is_swap(*pte)) {
            if (level_out) *level_out = 0;
            if (base_out) *base_out = va & ~(vaddr_t)(PAGE_SIZE - 1);
            if (size_out) *size_out = PAGE_SIZE;
            return pte;
        }
#endif
        if (!(*pte & PTE_V)) {
            /* Report the entire absent subtree so teardown can skip sparse
             * reservations without walking every base page under mm->lock. */
            size_t sz = pt_level_size(level);
            if (level_out) *level_out = level;
            if (base_out) *base_out = va & ~(vaddr_t)(sz - 1);
            if (size_out) *size_out = sz;
            return NULL;
        }
        if (arch_pte_is_leaf(*pte)) {
            size_t sz = pt_level_size(level);
            if (level_out) *level_out = level;
            if (base_out) *base_out = va & ~(vaddr_t)(sz - 1);
            if (size_out) *size_out = sz;
            return pte;
        }
        if (level == 0)
            return NULL;
        table = arch_pte_to_ptr(*pte);
    }
    return NULL;
}

int mm_query_leaf(pt_root_t *pgdir, vaddr_t va, mm_leaf_info_t *out) {
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!pgdir)
        return 0;

    int level = 0;
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(pgdir, va, &level, &base, &size);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte))
        return 0;

    out->level = level;
    out->base = base;
    out->size = size;
    out->pa = arch_pte_addr(*pte) + (va - base);
    out->flags = arch_pte_flags(*pte);
    out->dirty = (*pte & PTE_D) != 0;
    return 1;
}

uint64_t mm_pagemap_entry(pt_root_t *pgdir, vaddr_t va) {
    mm_leaf_info_t info;
    if (!mm_query_leaf(pgdir, va, &info))
        return 0;
    uint64_t pfn = info.pa / PAGE_SIZE;
    return (1ULL << 63) | (pfn & 0x7FFFFFFFFFFFFULL);
}

int mm_query_leaf_kaddr(pt_root_t *pgdir, vaddr_t va, void **kaddr_out,
                        size_t *avail_out) {
    if (!kaddr_out || !avail_out)
        return 0;
    *kaddr_out = NULL;
    *avail_out = 0;

    mm_leaf_info_t info;
    if (!mm_query_leaf(pgdir, va, &info))
        return 0;
    *kaddr_out = (void *)(info.pa + PAGE_OFFSET);
    *avail_out = info.size - (va - info.base);
    return 1;
}

int mm_fetch_user_insn32(pt_root_t *pgdir, vaddr_t va, uint32_t *out) {
    if (!out)
        return 0;
    void *kaddr = NULL;
    size_t avail = 0;
    if (!mm_query_leaf_kaddr(pgdir, va, &kaddr, &avail) || avail < sizeof(uint32_t))
        return 0;
    *out = *(uint32_t *)kaddr;
    return 1;
}

int mm_mark_leaf_dirty_if_writable(pt_root_t *pgdir, vaddr_t va) {
    if (!pgdir)
        return -1;
    size_t leaf_size = 0;
    pte_t *pte = pt_lookup_leaf(pgdir, va, NULL, NULL, &leaf_size);
    if (!pte || !(*pte & PTE_V) || !(*pte & PTE_W))
        return -1;
    pte_t flags = arch_pte_flags(*pte) | PTE_D;
#ifdef ARCH_HAS_PTE_BLOCK
    if (leaf_size >= PMD_SIZE)
        *pte = arch_pte_block(arch_pte_addr(*pte), flags);
    else
#endif
    *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
    arch_tlb_flush_page(va);
    return 0;
}

int mm_debug_pte_value(pt_root_t *pgdir, vaddr_t va, uintptr_t *slot_out,
                       pte_t *value_out) {
    if (slot_out)
        *slot_out = 0;
    if (value_out)
        *value_out = 0;
    if (!pgdir)
        return 0;
    pte_t *pte = pt_lookup_leaf(pgdir, va, NULL, NULL, NULL);
    if (slot_out)
        *slot_out = (uintptr_t)pte;
    if (value_out)
        *value_out = pte ? *pte : 0;
    return pte != NULL;
}

int pt_map(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags) {
    return pt_map_cls(pgdir, va, pa, flags, MM_ST_ANON_MAPPED);
}

/* Shared leaf installation keeps scalar and bounded fork mapping identical:
 * replace-reference handling, executable cache maintenance, PTE encoding and
 * the status sidecar all live in this one operation. `owner` is the table
 * returned with `pte` by the walk; callers retain the existing locking rules. */
static void pt_install_leaf_cls(pte_t *owner, pte_t *pte, vaddr_t va,
                                paddr_t pa, pte_t flags, uint8_t cls)
{
    if (*pte & PTE_V) {
        paddr_t old_pa = arch_pte_addr(*pte);
        if (old_pa != pa) {
            int is_leaf = arch_pte_is_leaf(*pte);
            if (is_leaf)
                frame_put(phys_to_pfn(old_pa));
        }
    }
    /* Executable mappings may be populated through PAGE_OFFSET before being
     * installed at their user VA.  Synchronize RAM-backed code before the PTE
     * becomes visible so demand paging, fork/COW, VMO maps and ELF loading all
     * obey the same AArch64 I/D-cache contract. */
    if (flags & PTE_X) {
        pfn_t pfn = phys_to_pfn(pa);
        if (pfn_valid(pfn))
            arch_flush_icache_range(pfn_to_virt(pfn), PAGE_SIZE);
    }
    *pte = arch_pte_leaf(pa, flags);

    /* Record status in the exact table returned with this PTE. Reusing the
     * borrowed owner avoids a second root walk without changing lock rules. */
    if (owner)
        mm_pt_note_present(owner, 0, arch_pt_vpn(va, 0),
                           (uint8_t)(MM_ST_CLS_BYTE(cls) |
                                     (flags & PTE_COW ? MM_ST_COW_BIT : 0) |
                                     mm_pt_prot_bits(flags)));
}

static void pt_note_cache_unpublished(pt_meta_t *meta, int idx,
                                      page_cache_page_t *page)
{
    if (!meta || !page || mm_pt_meta_level(meta) != 0)
        return;
    if (!meta->cache_owner) {
        pfn_t pfn = pfa_alloc_flags(0, 0);
        if (pfn == PFN_NONE)
            return;
        meta->cache_owner = pfn_to_virt(pfn);
        memset(meta->cache_owner, 0, PAGE_SIZE);
    }
    meta->cache_owner[idx] = page;
}

/* pt_map_cls is pt_map with an explicit per-page status. The status is the
 * authoritative record of what this page is; the PTE records only the frame
 * and effective permissions. Callers that know more than a private anonymous
 * page (for example, file-backed, fork-shared, or VMO pages) pass that class
 * so a later fault needs no interval lookup. */
int pt_map_cache_cls(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags,
                     uint8_t cls, page_cache_page_t *page)
{
    pte_t *owner = NULL;
    pte_t *pte = pt_walk_with_owner(pgdir, va, 1, &owner);
    if (!pte) return -ENOMEM;
    pt_install_leaf_cls(owner, pte, va, pa, flags, cls);
    if (page)
        pt_note_cache_unpublished(mm_pt_meta(owner), arch_pt_vpn(va, 0), page);
    return 0;
}

int pt_map_cls(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags,
               uint8_t cls)
{
    pte_t *owner = NULL;
    pte_t *pte = pt_walk_with_owner(pgdir, va, 1, &owner);
    if (!pte) return -ENOMEM;
    pt_install_leaf_cls(owner, pte, va, pa, flags, cls);
    return 0;
}

/* Map a complete, bounded child-fork batch after validating every input and
 * destination.  The child is unpublished and exclusively owned by mm_fork;
 * callers retain the 64 frame pins and account RSS only after success.  Empty
 * intermediate tables allocated by the one walk belong to that child and are
 * reclaimed by ordinary mm_destroy if preflight fails. */
int pt_map_anon_batch(pt_root_t *pgdir, vaddr_t start,
                      const struct a20_cow_anon_batch_page *pages,
                      size_t nr, int shared)
{
    if (!pgdir || !pages || nr == 0 || nr > A20_COW_ANON_BATCH_PAGES ||
        (start & (PAGE_SIZE - 1)) || start >= USER_VA_LIMIT ||
        nr > ((uintptr_t)-1 - start) / PAGE_SIZE)
        return -EINVAL;

    uintptr_t end = start + nr * PAGE_SIZE;
    if (end > USER_VA_LIMIT)
        return -EINVAL;

    int first = arch_pt_vpn(start, 0);
    int entries = arch_pt_level_entries(0);
    if (first < 0 || entries <= 0 || first >= entries ||
        nr > (size_t)(entries - first))
        return -EINVAL;
    /* The page indexes are contiguous and the run cannot wrap; matching each
     * endpoint's upper indexes therefore proves the complete span stays in
     * this same level-0 table on every paging geometry. */
    vaddr_t last = start + (nr - 1) * PAGE_SIZE;
    for (int level = 1; level <= ARCH_PT_ROOT_LEVEL; level++) {
        int first_parent = arch_pt_vpn(start, level);
        if (arch_pt_vpn(last, level) != first_parent)
            return -EINVAL;
    }

    for (size_t i = 0; i < nr; i++) {
        uint32_t cls = pages[i].class;
        if (!cls) {
            if (pages[i].pfn || pages[i].flags || pages[i].pcp || pages[i].kind)
                return -EINVAL;
            continue;
        }
        int shared_class = cls == MM_ST_ANON_SHARED || cls == MM_ST_FILE_SHARED;
        if (!pfn_valid((pfn_t)pages[i].pfn) ||
            (cls != MM_ST_ANON_MAPPED && cls != MM_ST_FILE_PRIVATE &&
             !shared_class) || !(pages[i].flags & PTE_U) ||
            (!shared && (pages[i].flags & PTE_W)))
            return -EINVAL;
    }

    pte_t *owner = NULL;
    pte_t *first_slot = pt_walk_with_owner(pgdir, start, 1, &owner);
    if (!first_slot || !owner)
        return -ENOMEM;
    pt_meta_t *meta = mm_pt_meta(owner);
    if (!meta || mm_pt_meta_level(meta) != 0)
        return -EINVAL;
    if (first_slot != &owner[first])
        return -EINVAL;

    /* Check raw entries as well as decoded leaves so swap entries, huge
     * leaves and nonzero invalid/reserved encodings all decline preflight. */
    for (size_t i = 0; i < nr; i++) {
        if (owner[first + (int)i] != 0)
            return -EEXIST;
    }

    /* No operation below this point can fail.  The caller's frame references
     * become child mapping references only after this entire function returns. */
    for (size_t i = 0; i < nr; i++) {
        if (!pages[i].class)
            continue;
        vaddr_t va = start + i * PAGE_SIZE;
        /* PFNs index compact allocator metadata; a range base is required to
         * recover the corresponding physical address. */
        paddr_t pa = pfn_to_phys((pfn_t)pages[i].pfn);
        pt_install_leaf_cls(owner, &owner[first + (int)i], va, pa,
                            (pte_t)pages[i].flags,
                            (uint8_t)pages[i].class);
        if (pages[i].pcp) {
            if (!meta->cache_owner) {
                pfn_t owner_pfn = pfa_alloc_flags(0, 0);
                if (owner_pfn != PFN_NONE) {
                    meta->cache_owner = pfn_to_virt(owner_pfn);
                    memset(meta->cache_owner, 0, PAGE_SIZE);
                }
            }
            if (meta->cache_owner)
                meta->cache_owner[first + (int)i] = pages[i].pcp;
        }
    }
    return 0;
}

int pt_map_huge(mm_struct_t *mm, vaddr_t va, paddr_t pa, pte_t flags,
                uint8_t cls) {
#ifdef ARCH_NO_PMD_LEAF
    (void)mm;
    (void)va;
    (void)pa;
    (void)flags;
    (void)cls;
    return -EOPNOTSUPP;
#else
    if (!mm || !mm->pgdir) return -EINVAL;
    pt_root_t *pgdir = mm->pgdir;
    if ((va & (PMD_SIZE - 1)) || (pa & (PMD_SIZE - 1)))
        return -EINVAL;

    pte_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 1; level--) {
        int idx = arch_pt_vpn(va, level);
        pte_t pte = table[idx];
        if (pte & PTE_V) {
            if (arch_pte_is_leaf(pte))
                return -EEXIST;
            table = arch_pte_to_ptr(pte);
        } else {
            /* MM-04 stage 1: same intermediate-node failure as pt_walk. */
            if (mm_fail_inject_scoped(MM_FAIL_STAGE_PT_NODE))
                return -ENOMEM;
            pte_t *next = (pte_t *)frame_alloc();
            if (!next) return -ENOMEM;
            mm_pt_node_init(next, level - 1);
            table[idx] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
            mm_pt_note_present(table, level, idx,
                               MM_ST_CLS_BYTE(MM_ST_PT_NODE));
            table = next;
        }
    }

    pte_t *pte = &table[arch_pt_vpn(va, 1)];
    if (*pte & PTE_V) {
        /* A level-0 table left behind by earlier 4K faulting in this 2 MiB
         * block used to refuse the huge leaf with -EEXIST unconditionally --
         * and since pt_lookup_leaf() only reports the (still-absent) 4K
         * entry, the caller could not tell that apart either.  Measured on
         * smoke-mm-stress: every single THP decline in every huge phase hit
         * an EMPTY child (live-children=0), so on a reused address space the
         * THP path never fired at all and every huge gate passed on 4K
         * pages.  An empty child is retired under the parent-then-child node
         * locks (the preorder the cursor uses) and the huge leaf installed;
         * a child holding any live or swapped entry still refuses, because a
         * 2 MiB frame would cover pages it does not own. */
        if (arch_pte_is_leaf(*pte))
            return -EEXIST;
        pte_t *l0 = arch_pte_to_ptr(*pte);
        if (!mm_pt_meta(l0))
            return -EEXIST;
        int idx1 = arch_pt_vpn(va, 1);
        mm_pt_node_lock(table);
        mm_pt_node_lock(l0);
        if (!pt_table_empty(l0, 0, -1)) {
            mm_pt_node_unlock(l0);
            mm_pt_node_unlock(table);
            return -EEXIST;
        }
        *pte = 0;
        mm_pt_node_clear_seg(table, 1, idx1);
        mm_pt_note_absent(table, 1, idx1);
        mm_pt_node_unlock(l0);
        mm_pt_node_unlock(table);
        /* Deferred reclamation: a lockless fault may already hold this
         * table's pointer; stale + grace period, never a direct free. */
        mm_pt_retire_table(mm, l0, 0);
    }
#ifdef ARCH_HAS_PTE_BLOCK
    if (flags & PTE_X) {
        pfn_t pfn = phys_to_pfn(pa);
        if (pfn_valid(pfn))
            arch_flush_icache_range(pfn_to_virt(pfn), PMD_SIZE);
    }
#endif
#ifdef ARCH_HAS_PTE_BLOCK
    *pte = arch_pte_block(pa, flags);
#else
    *pte = arch_pte_leaf(pa, flags);
#endif
    /* A huge leaf is one entry of THIS table, so its status slot lives here
     * at level 1, not at level 0 -- there is no level-0 table under it.  A
     * present leaf whose slot says INVALID is exactly what the auditor's
     * present_mismatch counts, and before this note the THP path produced
     * one on every huge fault. */
    mm_pt_note_present(table, 1, arch_pt_vpn(va, 1),
                       (uint8_t)(MM_ST_CLS_BYTE(cls) |
                                 (flags & PTE_COW ? MM_ST_COW_BIT : 0) |
                                 mm_pt_prot_bits(flags)));
    mm_huge_install_count++;
    return 0;
#endif
}

static int pt_entry_mapped(pte_t entry) {
    return (entry & PTE_V)
#ifdef CONFIG_SWAP
        || pte_is_swap(entry)
#endif
        ;
}

static int pt_table_empty(pte_t *table, int level, int cleared_idx) {
    int entries = arch_pt_level_entries(level);
    /* Sequential teardown normally leaves the next slot populated.  Check
     * its neighbours first, rather than rescanning an ever-longer cleared
     * prefix for every page.  These are only early nonempty answers: sparse
     * tables and the last leaf still get the full scan before retirement. */
    if (cleared_idx >= 0 && cleared_idx < entries) {
        if (cleared_idx + 1 < entries && pt_entry_mapped(table[cleared_idx + 1]))
            return 0;
        if (cleared_idx > 0 && pt_entry_mapped(table[cleared_idx - 1]))
            return 0;
    }
    for (int i = 0; i < entries; i++) {
        if (pt_entry_mapped(table[i]))
            return 0;
    }
    return 1;
}

/* Unmap a base page and retire empty tables. Even an absent leaf may have
 * an allocated prefix left by a failed pt_walk(); prune that prefix while
 * retaining the historical -EINVAL result for an absent mapping.
 * Caller holds mm->lock and the address-space TLB transaction. */
int pt_unmap(mm_struct_t *mm, vaddr_t va) {
    if (!mm) return -EINVAL;
    pte_t *path[ARCH_PT_ROOT_LEVEL + 1];
    int idx_path[ARCH_PT_ROOT_LEVEL + 1];
    pte_t *table = mm->pgdir;
    path[ARCH_PT_ROOT_LEVEL] = table;
    int level = ARCH_PT_ROOT_LEVEL;
    for (; level > 0; level--) {
        int idx = arch_pt_vpn(va, level);
        idx_path[level] = idx;
        pte_t pte = table[idx];
        if (!(pte & PTE_V)) break;
        if (arch_pte_is_leaf(pte)) return -EINVAL;
        table = arch_pte_to_ptr(pte);
        path[level - 1] = table;
    }
    int result = -EINVAL;
    int idx = arch_pt_vpn(va, level);
    if (level == 0) {
        mm_pt_node_lock(table);
        if ((table[idx] & PTE_V) && arch_pte_is_leaf(table[idx])) {
            table[idx] = 0;
            mm_pt_note_absent(table, 0, idx);
            result = 0;
        }
        mm_pt_node_unlock(table);
    }
    for (; level < ARCH_PT_ROOT_LEVEL; level++) {
        pte_t *child = path[level];
        pte_t *parent = path[level + 1];
        /* Parent lock spans emptiness and unlink, excluding a descended
         * cursor from inserting into the child between these two actions. */
        mm_pt_node_lock(parent);
        if (!pt_table_empty(child, level, idx)) {
            mm_pt_node_unlock(parent);
            break;
        }
        idx = idx_path[level + 1];
        parent[idx] = 0;
        mm_pt_node_clear_seg(parent, level + 1, idx);
        mm_pt_note_absent(parent, level + 1, idx);
        mm_pt_node_unlock(parent);
        mm_pt_retire_table(mm, child, level);
    }
    /* Device/VMO callers own their frame references. Never release a data
     * frame here; retirement only consumes empty intermediate table pages. */
    return result;
}

int pt_unmap_leaf(mm_struct_t *mm, vaddr_t va, paddr_t *pa_out,
                  vaddr_t *base_out, size_t *size_out, int *level_out) {
    if (!mm) return -EINVAL;
    pt_root_t *pgdir = mm->pgdir;
    pte_t *path[ARCH_PT_ROOT_LEVEL + 1];
    int idx_path[ARCH_PT_ROOT_LEVEL + 1];
    pte_t *table = pgdir;

    path[ARCH_PT_ROOT_LEVEL] = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
        int idx = arch_pt_vpn(va, level);
        idx_path[level] = idx;
        pte_t *pte = &table[idx];
#ifdef CONFIG_SWAP
        if (pte_is_swap(*pte)) {
            if (level != 0)
                return -EINVAL;
            mm_pt_node_lock(path[level]);
            if (!pte_is_swap(*pte)) {
                mm_pt_node_unlock(path[level]);
                return -EINVAL;
            }
            swap_free(pte_to_swp_entry(*pte));
            *pte = 0;
            mm_pt_note_absent(path[level], level, idx_path[level]);
            mm_pt_node_unlock(path[level]);
            if (pa_out) *pa_out = 0;
            if (base_out) *base_out = va & ~(vaddr_t)(PAGE_SIZE - 1);
            if (size_out) *size_out = PAGE_SIZE;
            if (level_out) *level_out = 0;
            return 0;
        }
#endif
        if (!(*pte & PTE_V))
            return -EINVAL;
        if (arch_pte_is_leaf(*pte)) {
            size_t sz = pt_level_size(level);
            vaddr_t base = va & ~(vaddr_t)(sz - 1);
            paddr_t pa;
            mm_pt_node_lock(path[level]);
            if (!(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) {
                mm_pt_node_unlock(path[level]);
                return -EINVAL;
            }
            pa = arch_pte_addr(*pte);
            *pte = 0;
            mm_pt_note_absent(path[level], level, idx_path[level]);
            mm_pt_node_unlock(path[level]);

            for (int l = level; l < ARCH_PT_ROOT_LEVEL; l++) {
                pte_t *child = path[l];
                pte_t *parent = path[l + 1];
                /* Test and clear in one critical section -- see pt_unmap(). */
                mm_pt_node_lock(parent);
                if (!pt_table_empty(child, l, idx_path[l])) {
                    mm_pt_node_unlock(parent);
                    break;
                }
                parent[idx_path[l + 1]] = 0;
                mm_pt_node_clear_seg(parent, l + 1, idx_path[l + 1]);
                mm_pt_note_absent(parent, l + 1, idx_path[l + 1]);
                mm_pt_node_unlock(parent);
                mm_pt_retire_table(mm, child, l);
            }

            if (pa_out) *pa_out = pa;
            if (base_out) *base_out = base;
            if (size_out) *size_out = sz;
            if (level_out) *level_out = level;
            return 0;
        }
        if (level == 0)
            return -EINVAL;
        table = arch_pte_to_ptr(*pte);
        path[level - 1] = table;
    }
    return -EINVAL;
}

/* Run-batched teardown for sequential final-destroy loops (free_vma_pages).
 *
 * pt_unmap_leaf() re-walks from the root for every page and, after clearing
 * each leaf, walks the empty-table cascade with a node lock per level.  A
 * sequential sweep of a mapped region therefore pays several lock round
 * trips per page for state that only changes once per table.  This helper
 * descends once, holds the level-0 node lock across the whole run, clears
 * each present base-page leaf, and runs the cascade a single time at the
 * end (the last cleared index feeds pt_table_empty()'s neighbour hint).
 * Everything the scalar path treats specially -- swap entries, huge leaves,
 * absent slots, absent upper subtrees -- ends the run and is left for the
 * caller's per-page fallback, so per-page semantics never change. */
int pt_unmap_leaf_run_owned(struct mm_struct *mm, vaddr_t va, int max_pages,
                            paddr_t *pa_out, void **owner_out) {
    if (!mm || !mm->pgdir || !pa_out || max_pages <= 0) return 0;
    pt_root_t *pgdir = mm->pgdir;
    pte_t *path[ARCH_PT_ROOT_LEVEL + 1];
    int idx_path[ARCH_PT_ROOT_LEVEL + 1];
    pte_t *table = pgdir;

    path[ARCH_PT_ROOT_LEVEL] = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
        int idx = arch_pt_vpn(va, level);
        idx_path[level] = idx;
        pte_t *pte = &table[idx];
        pte_t entry = *pte;
#ifdef CONFIG_SWAP
        if (pte_is_swap(entry))
            return 0;
#endif
        if (!(entry & PTE_V))
            return 0;
        if (arch_pte_is_leaf(entry)) {
            /* Only base-page runs; huge leaves fall back to the scalar path
             * (free_vma_pages demotes straddlers there). */
            if (level != 0)
                return 0;
            break;
        }
        if (level == 0)
            return 0;
        table = arch_pte_to_ptr(entry);
        path[level - 1] = table;
    }

    int entries = arch_pt_level_entries(0);
    int first = idx_path[0];
    if (first < 0 || first >= entries)
        return 0;
    int count = 0;
    mm_pt_node_lock(path[0]);
    for (int i = 0; i < max_pages && first + i < entries; i++) {
        pte_t *pte = &path[0][first + i];
        pte_t entry = *pte;
#ifdef CONFIG_SWAP
        if (pte_is_swap(entry))
            break;
#endif
        if (!(entry & PTE_V) || !arch_pte_is_leaf(entry))
            break;
        pa_out[count] = arch_pte_addr(entry);
        if (owner_out)
            owner_out[count] = mm_pt_cache_peek(path[0], first + i);
        count++;
        *pte = 0;
        mm_pt_note_absent(path[0], 0, first + i);
    }
    mm_pt_node_unlock(path[0]);
    if (!count)
        return 0;

    /* The run emptied its slots; cascade absent upper nodes exactly like the
     * per-page path, once for the whole run. */
    for (int l = 0; l < ARCH_PT_ROOT_LEVEL; l++) {
        pte_t *child = path[l];
        pte_t *parent = path[l + 1];
        mm_pt_node_lock(parent);
        if (!pt_table_empty(child, l, first + count - 1)) {
            mm_pt_node_unlock(parent);
            break;
        }
        parent[idx_path[l + 1]] = 0;
        mm_pt_node_clear_seg(parent, l + 1, idx_path[l + 1]);
        mm_pt_note_absent(parent, l + 1, idx_path[l + 1]);
        mm_pt_node_unlock(parent);
        mm_pt_retire_table(mm, child, l);
    }
    return count;
}

int pt_unmap_leaf_run(struct mm_struct *mm, vaddr_t va, int max_pages,
                      paddr_t *pa_out)
{
    return pt_unmap_leaf_run_owned(mm, va, max_pages, pa_out, NULL);
}

paddr_t pt_translate(pt_root_t *pgdir, vaddr_t va) {
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(pgdir, va, NULL, &base, &size);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) return 0;
    return arch_pte_addr(*pte) + (va - base);
}

/* Copy the kernel-space mappings into the new page table; kernel space is
 * shared.  On LoongArch the kernel space is translated directly by the DMW,
 * bypassing the TLB and the multi-level page table walk entirely, so this can
 * be left empty to save the copy. */
void pt_map_kernel(pt_root_t *pgdir) {
    for (int i = ARCH_PT_USER_END; i < ARCH_PT_ENTRIES; i++) {
        if (boot_pgdir[i] & PTE_V) {
            pgdir[i] = boot_pgdir[i];
            /* These nodes are shared with every other address space and are
             * owned by the boot page table.  Mark them so a cursor can tell a
             * shared node from a private one; mm_addrspace_lock() refuses any
             * range that reaches them. */
            mm_pt_note_present(pgdir, ARCH_PT_ROOT_LEVEL, i,
                               MM_ST_CLS_BYTE(MM_ST_PT_NODE));
        }
    }
}

int pt_map_range(pt_root_t *pgdir, vaddr_t va, paddr_t pa, size_t size, pte_t flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        int r = pt_map(pgdir, va + off, pa + off, flags);
        if (r < 0) return r;
    }
    return 0;
}

static pte_t *pt_clone_level(pte_t *src, int level) {
    pte_t *dst = level == ARCH_PT_ROOT_LEVEL ?
        pt_create() : (pte_t *)frame_alloc();
    if (!dst) return NULL;

    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        pte_t pte = src[i];
        if (!(pte & PTE_V)) continue;
        if (level == ARCH_PT_ROOT_LEVEL && i >= ARCH_PT_USER_END) {
            dst[i] = pte;
            continue;
        }

        int is_leaf = arch_pte_is_leaf(pte);

        if (is_leaf) {
            size_t leaf_size = pt_level_size(level);
            int order = (leaf_size == PMD_SIZE) ? PMD_ORDER : 0;
            if (leaf_size != PAGE_SIZE && leaf_size != PMD_SIZE) {
                pt_destroy(dst);
                return NULL;
            }
            pfn_t nf = pfa_alloc(order);
            if (nf == PFN_NONE) { pt_destroy(dst); return NULL; }
            memcpy(pfn_to_virt(nf), arch_pte_to_ptr(pte), leaf_size);
#ifdef ARCH_HAS_PTE_BLOCK
            dst[i] = (leaf_size >= PMD_SIZE)
                         ? arch_pte_block(pfn_to_phys(nf), arch_pte_flags(pte))
                         : arch_pte_leaf(pfn_to_phys(nf), arch_pte_flags(pte));
#else
            dst[i] = arch_pte_leaf(pfn_to_phys(nf), arch_pte_flags(pte));
#endif
        } else {
            pte_t *next_src = arch_pte_to_ptr(pte);
            pte_t *next_dst = pt_clone_level(next_src, level - 1);
            if (!next_dst) { pt_destroy(dst); return NULL; }
            dst[i] = arch_pte_from_pa(va_to_pa(next_dst)) | PTE_DIR;
        }
    }
    /* A clone must reproduce the source's per-page status exactly; the
     * auditor compares the two representations, so a partial copy here is
     * precisely the divergence it exists to catch. */
    mm_pt_meta_clone(dst, src, level);
    return dst;
}

pte_t *pt_clone(pt_root_t *src_pgdir) {
    if (!src_pgdir) return NULL;
    return pt_clone_level(src_pgdir, ARCH_PT_ROOT_LEVEL);
}

static void pt_destroy_user_recursive(mm_struct_t *mm, pte_t *table, int level,
                                      vaddr_t base) {
    if (!table) return;
    pfn_t put_batch[64];
    size_t put_count = 0;
    /* Only user half (0..255) lives at root; kernel half is shared
     * and must not be freed.  Lower levels may span all 512 entries. */
    int limit = (level == ARCH_PT_ROOT_LEVEL) ?
        ARCH_PT_USER_END : arch_pt_level_entries(level);
    for (int i = 0; i < limit; i++) {
        pte_t pte = table[i];
#ifdef CONFIG_SWAP
        if (pte_is_swap(pte)) {
            if (put_count) {
                frame_put_many(put_batch, put_count);
                put_count = 0;
            }
            swap_free(pte_to_swp_entry(pte));
            table[i] = 0;
            continue;
        }
#endif
        if (!(pte & PTE_V)) continue;

        int is_leaf = arch_pte_is_leaf(pte);
        vaddr_t va = base | ((vaddr_t)i << (ARCH_PT_BITS * level));

        if (is_leaf) {
            pfn_t pfn = phys_to_pfn(arch_pte_addr(pte));
            page_cache_page_t *owner = mm_pt_cache_peek(table, i);
            if (owner) {
                page_cache_put(owner);
                table[i] = 0;
                continue;
            }
            if (mm) {
                if (MM_ST_GET_CLASS(mm_pt_peek(table, level, i)) ==
                    MM_ST_FILE_PRIVATE) {
                    mm_seg_t *vma = mm->mmap;
                    while (vma && (va < vma->start || va >= vma->end))
                        vma = vma->next;
                    page_cache_page_t *page =
                        mm_file_cache_mapping_get(vma, va, pfn);
                    if (page) {
                        page_cache_put(page);
                        page_cache_put(page);
                        table[i] = 0;
                        continue;
                    }
                }
            }
            /* Ownership follows the user address range, including protected
             * leaves whose user-access permission is temporarily cleared. */
            put_batch[put_count++] = pfn;
            table[i] = 0;
            if (put_count == 64) {
                frame_put_many(put_batch, put_count);
                put_count = 0;
            }
        } else {
            if (put_count) {
                frame_put_many(put_batch, put_count);
                put_count = 0;
            }
            paddr_t next_pa = arch_pte_addr(pte);
            pfn_t next_pfn = phys_to_pfn(next_pa);
            if ((next_pa & (PAGE_SIZE - 1)) || !pfn_valid(next_pfn)) {
                kerr("pt_destroy_user: skip invalid non-leaf level=%d idx=%d pte=0x%lx pa=0x%lx\n",
                     level, i, (unsigned long)pte, (unsigned long)next_pa);
                table[i] = 0;
                continue;
            }
            pte_t *next = arch_pte_to_ptr(pte);
            pt_destroy_user_recursive(mm, next, level - 1, va);
            pt_free_table(next, level - 1);
            table[i] = 0;
            mm_pt_note_absent(table, level, i);
        }
    }
    if (put_count)
        frame_put_many(put_batch, put_count);
}

void pt_destroy_user(pt_root_t *pgdir) {
    if (!pgdir) return;
    pt_destroy_user_recursive(NULL, pgdir, ARCH_PT_ROOT_LEVEL, 0);
    pt_free_table(pgdir, ARCH_PT_ROOT_LEVEL);
}

void pt_destroy_user_mm(mm_struct_t *mm) {
    if (!mm || !mm->pgdir) return;
    pt_root_t *pgdir = mm->pgdir;
    pt_destroy_user_recursive(mm, pgdir, ARCH_PT_ROOT_LEVEL, 0);
    pt_free_table(pgdir, ARCH_PT_ROOT_LEVEL);
}

#endif /* defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU) */

static inline int user_range_ok(uint64_t va, size_t n) {
    va = (uint64_t)(vaddr_t)va;
    if (n == 0)
        return 1;
    if (va >= USER_VA_LIMIT)
        return 0;
    return n <= USER_VA_LIMIT - va;
}

int user_prepare_write(struct task_t *t, uint64_t va)
{
    if (!t || !t->mm)
        return -EFAULT;
    if (!user_range_ok(va, 1))
        return -EFAULT;
#ifdef CONFIG_NOMMU
    return 0;
#else
    if (!t->pgdir)
        return -EFAULT;
    va = (uint64_t)(vaddr_t)va;
    pte_t *pte = pt_lookup_leaf(t->pgdir, va, NULL, NULL, NULL);
    if (!pte || !(*pte & PTE_V)) {
        int r = handle_demand_fault(t, va);
        if (r < 0)
            return -EFAULT;
        pte = pt_lookup_leaf(t->pgdir, va, NULL, NULL, NULL);
        if (!pte || !(*pte & PTE_V))
            return -EFAULT;
    }
    if (!arch_pte_is_leaf(*pte) || !(*pte & PTE_U))
        return -EFAULT;
    if (!(*pte & PTE_W)) {
        int r = handle_cow_fault(t, va);
        if (r < 0)
            return -EFAULT;
        pte = pt_lookup_leaf(t->pgdir, va, NULL, NULL, NULL);
        if (!pte || !(*pte & PTE_V))
            return -EFAULT;
    }
    if (!pte_user_writable(*pte))
        return -EFAULT;
    return 0;
#endif
}

/* A direct-map address is borrowed from the PTE, not a pinned translation.
 * Keep the mapping stable through the copy: fork/COW, munmap and mremap may
 * otherwise replace it after lookup and before memcpy. Fault resolution can
 * sleep, so it runs outside the lock and the next iteration re-reads the PTE.
 * Limit each locked access to one base page, including huge mappings. */

/* Rust owns the page-at-a-time copy loop. These narrow C operations retain
 * page-table/VMA policy and the existing fault resolver. A successful access
 * leaves this exact mm locked; retry requests are always returned unlocked. */
int a20_usercopy_page_access(task_t *t, mm_struct_t *mm, uintptr_t address,
                             int write, void **kernel_address, size_t *chunk,
                             uint64_t *irq_flags, unsigned *preempt_before,
                             unsigned *shared_write_guard_out)
{
#ifdef CONFIG_NOMMU
    if (!t || !mm || !kernel_address || !chunk || !irq_flags || !preempt_before ||
        !shared_write_guard_out ||
        (write != 0 && write != 1))
        return -EFAULT;
    uint64_t user_va = (uint64_t)(vaddr_t)address;
    if (!user_range_ok(user_va, 1))
        return -EFAULT;
    /* NOMMU user addresses are identity-mapped. There is no page-table lock
     * or demand-fault path; the Rust loop still advances one page at a time
     * through this adapter so it shares the bounded-copy and string logic. */
    *kernel_address = (void *)(uintptr_t)user_va;
    size_t page_chunk = PAGE_SIZE - (user_va & (PAGE_SIZE - 1));
    size_t user_chunk = (size_t)(USER_VA_LIMIT - user_va);
    *chunk = page_chunk < user_chunk ? page_chunk : user_chunk;
    *irq_flags = arch_irqs_enabled() ? 1 : 0;
    *preempt_before = preempt_count();
    *shared_write_guard_out = 0;
    return 0;
#else
    if (!t || !mm || !kernel_address || !chunk || !irq_flags || !preempt_before ||
        !shared_write_guard_out ||
        (write != 0 && write != 1))
        return -EFAULT;
    uint64_t user_va = (uint64_t)(vaddr_t)address;
    if (!user_range_ok(user_va, 1))
        return -EFAULT;
    uint64_t original_irq = arch_irqs_enabled() ? 1 : 0;
    unsigned original_preempt = preempt_count();
    *irq_flags = original_irq;
    *preempt_before = original_preempt;
    unsigned shared_write_guard = 0;
    uint64_t task_flags;

    /* park_lock owns task->mm. Nest it before the address-space lock, then
     * release it while the exact pinned mm stays locked for the copy. */
retry_access:
    task_flags = spin_lock_irqsave(&t->park_lock);
    if (t->mm != mm || !mm->pgdir || t->pgdir != mm->pgdir) {
        spin_unlock_irqrestore(&t->park_lock, task_flags);
        if (shared_write_guard)
            mm_shared_write_leave();
#ifdef CONFIG_USER_COPY_TEST
        if (arch_irqs_enabled() != (int)original_irq ||
            preempt_count() != original_preempt)
            panic("usercopy task-binding lock state");
#endif
        return -EFAULT;
    }
    /* Interrupts are already disabled by park_lock's irqsave. Acquire the
     * nested MM lock without a second irqsave token, then hand the original
     * token to the eventual page guard. Restoring task_flags here would enable
     * interrupts while mm->lock is still held. */
    spin_lock(&mm->lock);
    spin_unlock(&t->park_lock);

    uint64_t va = user_va;
    vaddr_t base;
    size_t mapped_size;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, &base, &mapped_size);
    int allowed = pte && (write ? pte_user_writable(*pte)
                                : pte_user_readable(*pte));
    mm_seg_t *vma = mm_seg_find(mm, va);
    if (vma && !(vma->vm_flags & (write ? VM_WRITE : VM_READ))) {
        spin_unlock_irqrestore(&mm->lock, task_flags);
        if (shared_write_guard)
            mm_shared_write_leave();
#ifdef CONFIG_USER_COPY_TEST
        if (arch_irqs_enabled() != (int)original_irq ||
            preempt_count() != original_preempt)
            panic("usercopy permission lock state");
#endif
        return -EFAULT;
    }
    int needs_gate = write && allowed && vma &&
        (vma->vm_flags & (VM_FILE | VM_SHARED | VM_WRITE)) ==
            (VM_FILE | VM_SHARED | VM_WRITE);
    if (needs_gate && !shared_write_guard) {
        spin_unlock_irqrestore(&mm->lock, task_flags);
        if (mm_shared_write_enter_wait() < 0)
            return -EFAULT;
        shared_write_guard = 1;
        goto retry_access;
    }
    if (!allowed) {
        spin_unlock_irqrestore(&mm->lock, task_flags);
        if (shared_write_guard)
            mm_shared_write_leave();
#ifdef CONFIG_USER_COPY_TEST
        if (arch_irqs_enabled() != (int)original_irq ||
            preempt_count() != original_preempt)
            panic("usercopy retry lock state");
#endif
        return 1; /* Rust resolves faults only after this lock is released. */
    }

    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) {
        spin_unlock_irqrestore(&mm->lock, task_flags);
        if (shared_write_guard)
            mm_shared_write_leave();
#ifdef CONFIG_USER_COPY_TEST
        if (arch_irqs_enabled() != (int)original_irq ||
            preempt_count() != original_preempt)
            panic("usercopy stale-PTE lock state");
#endif
        return 1;
    }
#ifdef CONFIG_USER_COPY_TEST
#ifdef CONFIG_KERNEL_PREEMPT
    if (arch_irqs_enabled() || preempt_count() != original_preempt + 1)
#else
    if (arch_irqs_enabled() || preempt_count() != original_preempt)
#endif
        panic("usercopy page guard acquire state");
#endif
    (void)mapped_size;
    *kernel_address = (void *)(uintptr_t)(arch_pte_addr(*pte) + PAGE_OFFSET + va - base);
    size_t page_chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
    size_t user_chunk = (size_t)(USER_VA_LIMIT - va);
    *chunk = page_chunk < user_chunk ? page_chunk : user_chunk;
    *irq_flags = task_flags;
    *pte |= PTE_A | (write ? PTE_D : 0);
    *shared_write_guard_out = shared_write_guard;
    return 0;
#endif
}

int a20_usercopy_resolve_fault(task_t *t, mm_struct_t *mm,
                               uintptr_t address, int write,
                               uint64_t irq_before, unsigned preempt_before)
{
    (void)irq_before;
    (void)preempt_before;
#ifdef CONFIG_NOMMU
    (void)t; (void)mm; (void)address; (void)write;
    return -EFAULT;
#else
    if (!t || !mm || (write != 0 && write != 1))
        return -EFAULT;
#ifdef CONFIG_USER_COPY_TEST
    if (arch_irqs_enabled() != (int)irq_before ||
        preempt_count() != preempt_before)
        panic("usercopy fault resolver entered with lock state");
#endif
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    int same_mm = t->mm == mm;
    spin_unlock_irqrestore(&t->park_lock, flags);
    if (!same_mm)
        return -EFAULT;
    return handle_demand_fault_access(t, (uint64_t)(vaddr_t)address,
                                      write ? MM_FAULT_ACCESS_WRITE
                                            : MM_FAULT_ACCESS_READ);
#endif
}

void a20_usercopy_page_unlock(mm_struct_t *mm, uint64_t irq_flags,
                              unsigned preempt_before,
                              unsigned shared_write_guard)
{
    (void)preempt_before;
#ifndef CONFIG_NOMMU
#ifdef CONFIG_USER_COPY_TEST
#ifdef CONFIG_KERNEL_PREEMPT
    if (arch_irqs_enabled() || preempt_count() != preempt_before + 1)
#else
    if (arch_irqs_enabled() || preempt_count() != preempt_before)
#endif
        panic("usercopy page guard release state");
#endif
    spin_unlock_irqrestore(&mm->lock, irq_flags);
    if (shared_write_guard)
        mm_shared_write_leave();
#ifdef CONFIG_USER_COPY_TEST
    if (arch_irqs_enabled() != (int)irq_flags ||
        preempt_count() != preempt_before)
        panic("usercopy page guard restore state");
#endif
#else
    (void)mm; (void)irq_flags; (void)preempt_before;
    if (shared_write_guard)
        mm_shared_write_leave();
#endif
}

extern long a20_usercopy_copy_from_rust(task_t *t, mm_struct_t *mm,
                                        void *dst, const void *src, size_t n);
extern long a20_usercopy_copy_to_rust(task_t *t, mm_struct_t *mm,
                                      void *dst, const void *src, size_t n);
extern int a20_usercopy_read_u32_atomic_rust(task_t *t, mm_struct_t *mm,
                                             uintptr_t src, uint32_t *out);
extern long a20_user_strncpy_rust(task_t *t, mm_struct_t *mm, char *dst,
                                  const char *src, size_t max);
extern long a20_user_strnlen_rust(task_t *t, mm_struct_t *mm,
                                  const char *src, size_t max);

static mm_struct_t *usercopy_pin_mm(task_t *t)
{
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    mm_struct_t *mm = mm_get(t->mm);
    spin_unlock_irqrestore(&t->park_lock, flags);
    return mm;
}


int user_read_u32_atomic(const uint32_t *src, uint32_t *out)
{
    task_t *t = proc_current();
    if (!t || !t->mm || !out)
        return -EFAULT;
    uintptr_t address = (uintptr_t)src;
    if ((address & (sizeof(uint32_t) - 1)) != 0 ||
        !user_range_ok((uint64_t)address, sizeof(uint32_t)))
        return -EFAULT;
    mm_struct_t *mm = usercopy_pin_mm(t);
    if (!mm)
        return -EFAULT;
    int result = a20_usercopy_read_u32_atomic_rust(t, mm, address, out);
    mm_destroy(mm);
    return result;
}

long copy_from_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    if (!user_range_ok((uint64_t)(uintptr_t)src, n)) return -EFAULT;
    mm_struct_t *mm = usercopy_pin_mm(t);
    if (!mm) return -EFAULT;
    long result = a20_usercopy_copy_from_rust(t, mm, dst, src, n);
    mm_destroy(mm);
    return result;
}

long copy_to_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    if (!user_range_ok((uint64_t)(uintptr_t)dst, n)) return -EFAULT;
    mm_struct_t *mm = usercopy_pin_mm(t);
    if (!mm) return -EFAULT;
    long result = a20_usercopy_copy_to_rust(t, mm, dst, src, n);
    mm_destroy(mm);
    return result;
}

long user_strncpy(char *dst, const char *src, size_t max) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    mm_struct_t *mm = usercopy_pin_mm(t);
    if (!mm) return -EFAULT;
    long result = a20_user_strncpy_rust(t, mm, dst, src, max);
    mm_destroy(mm);
    return result;
}

long user_strnlen(const char *src, size_t max) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    mm_struct_t *mm = usercopy_pin_mm(t);
    if (!mm) return -EFAULT;
    long result = a20_user_strnlen_rust(t, mm, src, max);
    mm_destroy(mm);
    return result;
}
