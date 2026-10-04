#ifndef _VM_H
#define _VM_H

#include "core/types.h"
#include "core/consts.h"
#include "core/refcount.h"
#include "core/lock.h"
#include "core/sync.h"
#include "mm/frame.h"

struct vmo;
struct vnode;
struct page_cache_page;

#define VM_READ      (1UL << 0)
#define VM_WRITE     (1UL << 1)
#define VM_EXEC      (1UL << 2)
#define VM_SHARED    (1UL << 3)
#define VM_ANON      (1UL << 4)
#define VM_STACK     (1UL << 5)
#define VM_GUARD     (1UL << 6)
#define VM_COW       (1UL << 7)
#define VM_FIXED     (1UL << 8)
#define VM_DONTFORK  (1UL << 9)
#define VM_WIPEONFORK (1UL << 10)
#define VM_HUGEPAGE  (1UL << 11)
#define VM_NOHUGEPAGE (1UL << 12)
#define VM_FILE      (1UL << 13)
#define VM_VMO       (1UL << 14)
#define VM_LOCKED    (1UL << 15)
#define VM_SYSV_SHM  (1UL << 16)
#define VM_PFNMAP    (1UL << 17)
/* mseal(2): the VMA is sealed against subsequent mmap-FIXED overwrite,
 * mprotect, munmap, mremap, madvise(DONTNEED/FREE/REMOVE) and unmap-then-
 * remap across its range.  Sealing is one-way for the life of the VMA and is
 * inherited by fork children sharing or copying the address space.  Enforced
 * in the core MM mutation paths (mm_mmap_locked / mm_mprotect_locked /
 * mm_munmap_locked / mm_mremap_locked), not in the ABI layer. */
#define VM_SEALED    (1UL << 18)

/* Typical compiler processes have O(100) mappings.  Keep a compact secondary
 * pointer index for binary-search lookup while retaining the linked list as
 * the ownership and mutation representation.
 *
 * rustc/LLVM address spaces routinely exceed the old 256-mapping cutoff; below
 * that the lookup degraded to a linear scan on every page fault.  1024 keeps
 * binary search for realistic compiler processes (8 KiB per mm_struct), one
 * entry per mapping. */
#define MM_SEG_INDEX_CAPACITY 1024

#ifdef CONFIG_NOMMU
#define NOMMU_ALLOC_MAX    32
#define NOMMU_ALLOC_IMAGE  0
#define NOMMU_ALLOC_STACK  1
#define NOMMU_ALLOC_TLS    2
#endif

#define MM_SEG_ANON 0u
#define MM_SEG_FILE 1u
#define MM_SEG_VMO  2u
#define MM_SEG_MAGIC 0x53454731u   /* "SEG1" */

/*
 * The mapping record.
 *
 * This used to be two structs: vm_area_t (the address-space interval, slab
 * allocated, refcounted, doubly linked) and mm_seg_t (the backing-object
 * descriptor the page-table node entries name, allocated a whole page frame at
 * a time).  They described the same thing and were kept in step by hand, which
 * is where four of the lifetime defects in roadmap 13.15 came from.
 *
 * They are one record now, and it is here rather than in pt.h because it is
 * written in terms of the VM_* bits defined just above.  The field names are
 * the VMA's, deliberately: the ~200 places that read v->start / v->vm_flags /
 * v->file_vnode became type renames rather than field renames, and the
 * segment's old spellings (base_va/len/flags/fd/offset/vnode) are gone rather
 * than kept as aliases.  pt.h forward-declares the type for the segment table
 * and the cursor API.
 *
 * The allocation moved from pfa_alloc_page() to the slab with the merge.  A
 * segment that is also a mapping is one per mmap, and one page frame per
 * anonymous mmap is not affordable; the page frame was sized for the far
 * rarer case of a name shared across many node entries.
 */
struct mm_seg;
typedef struct mm_seg mm_seg_t;

struct mm_seg {
    uint32_t         magic;
    /* One counter for the whole record, because there is one record.  It
     * counts three different things at once, which is what the two separate
     * refcounts used to divide between them and keep in step:
     *   - references held by page-table node entries (the segment table slots);
     *   - references held by cursors walking the address-space list;
     *   - references held by whoever created the mapping (the address-space
     *     list itself).
     * The release callback fires when the last of all three is gone, which is
     * exactly when the vnode/vmo reference must drop. */
    refcount_t       refcount;

    vaddr_t          start;      /* inclusive */
    vaddr_t          end;        /* exclusive */
    /* Offset into the backing object that `start` maps: the vnode for
     * VM_FILE, the vmo for VM_VMO.  The VMA kept these as two fields
     * (file_offset, vmo_offset) that were never both live -- mmap.c already
     * chose between them on VM_VMO -- so they are one field now. */
    uint64_t         backing_offset;
    uint64_t         vm_flags;   /* VM_* bits */
    /* Arch leaf flags for this mapping's protection.  Normally derivable from
     * the VM_* bits above via mm_vm_flags_to_pte_flags(), but carried
     * explicitly because mprotect writes pte_flags and vm_flags through two
     * paths and 17 of the mm_seg_find call sites read pte_flags directly. */
    pte_t            pte_flags;
    /* Policy metadata that is NOT backing-object state:
     *
     *   vmar_cap   native VMAR's ceiling -- protect() may not grant a bit that
     *              was not available when the VMAR was created.  0 means "no
     *              ceiling recorded" (every Linux-created mapping).
     *   sysv_shmid SysV shared memory identity, so a fault can name the shmid
     *              without walking back to a mapping record.
     *
     * VM_SEALED is not here: it is a VM_* bit, so it rides in vm_flags. */
    uint32_t         vmar_cap;
    int32_t          sysv_shmid; /* -1 = not SysV shared memory */
    int              file_fd;    /* FILE: where the vnode reference came from */
    struct vnode    *file_vnode; /* FILE, referenced by the record itself */
    struct vmo      *vmo;        /* VMO, referenced by the record itself */
    /* Drop the backing references.  Runs from the deferred-free queue only,
     * never from a page fault or an index rebuild, because it can block; see
     * `released` below.  A callback rather than a direct call because pt.c
     * must not know about vnodes. */
    void            (*release)(mm_seg_t *s);
    /* Set once `release` has run.  The last mm_seg_put() frees the record, and
     * by then the release may already have happened on the queue -- in which
     * case the free must not run it again, and in particular must not run it
     * from whatever context happened to drop the final page-table reference. */
    uint8_t          released;

    /* Address-space list linkage.  Order is by start, ascending. */
    struct mm_seg   *prev;
    struct mm_seg   *next;
    /* Deferred-free linkage, separate from `next` so a record on the deferred
     * list is never mistaken for one on mm->mmap. */
    struct mm_seg   *deferred_next;
#ifdef CONFIG_NOMMU
    void            *nommu_alloc; /* raw allocation backing this mapping */
#endif
};

/* The backing-object class, derived rather than stored.  The segment used to
 * carry a `kind` byte and a `shared` byte alongside the VM_* flags that already
 * determined both -- one more pair of facts kept in step by hand, which is the
 * failure mode the header comment above is about.  Everything that used to read
 * them asks here instead, so there is exactly one spelling of the answer. */
static inline uint8_t mm_seg_kind(const mm_seg_t *s)
{
    if (s->vm_flags & VM_VMO)
        return MM_SEG_VMO;
    if (s->vm_flags & VM_FILE)
        return MM_SEG_FILE;
    return MM_SEG_ANON;
}

static inline int mm_seg_shared(const mm_seg_t *s)
{
    return (s->vm_flags & VM_SHARED) != 0;
}

typedef struct mm_tlb_hold {
    struct mm_tlb_hold *next;
    pfn_t frame;
    struct page_cache_page *page;
    uint8_t kind;
    uint8_t pt_level;     /* MM_TLB_HOLD_PT: order to free the PT frame at */
} mm_tlb_hold_t;

/* A page-table page that has been unlinked from the tree.  The frame stays
 * marked FRAME_F_PT (so nothing can hand it out) until the grace period ends;
 * only then is the metadata dropped and the frame returned to the buddy. */
typedef struct mm_pt_retire {
    struct mm_pt_retire *next;
    pfn_t frame;
    uint8_t level;          /* order to free the frame at */
} mm_pt_retire_t;

/*
 * MM_AS_MODEL — the single-level memory model.
 *
 * There is one record per mapping (mm_seg_t, defined above), one list
 * (mm->mmap) and one ordered index over it (mm->seg_index).  What the target
 * model asked for, and where each piece lives:
 *
 *  - The authoritative per-virtual-page state is the per-PTE metadata array
 *    attached to the covering page-table page (pt_meta_t.cls[], mm/pt.h).
 *    The mapping record is an interval claim; the metadata is a per-page
 *    claim, and a page fault resolves from the latter.
 *  - All page-table programming goes through the transactional cursor
 *    (mm_addrspace_lock / mm_cursor_{query,map,mark,unmap}), whose unit of
 *    writer exclusion is the page-table-page lock.  Transactions over
 *    disjoint ranges do not serialize.
 *  - Status lives in a side array rather than in the PTE's software bits,
 *    because riscv32/arm32 have no free software bits at their root levels
 *    and loongarch64 aliases PTE_R/W/X onto its memory-attribute field.
 *
 * This file used to carry a second model beside this one.  It was two
 * representations of a mapping -- the VMA and the backing-object segment --
 * plus two interval indexes (vma_index[] and seg_index[]) and two lookups
 * (mm_find_vma() and mm_seg_find()) over them, kept in step by hand.  All of
 * it is now one record, one index and one lookup; what the two representations
 * disagreed about is checked by mm_pt_audit_addrspace(), which runs over every
 * live address space at shutdown.
 *
 * Still to come: fault dispatch consults the record for the backing object
 * rather than reading it out of the page-table node entry.  The node-entry
 * index (mm/pt.h) is the cache that makes that cheap, and it is already the
 * first thing a file fault tries.
 *
 * mm_struct_t lifetime and address-space invariants:
 * - refcount is shared by every task/thread that uses the same address space.
 *   A task_t may store mm == NULL only for kernel-only tasks or after teardown
 *   has detached it from user memory.
 * - mm_destroy() drops one reference. The final reference owns destruction of
 *   every mapping, its backing resources, the user page-table leaves, and the
 *   user page table itself; callers must not keep mapping or pgdir pointers
 *   after dropping the last reference.
 * - mmap is an ordered, non-overlapping mapping list owned by the mm.
 *   Insertion, removal, split, merge and release must preserve that, and must
 *   account total_vm/rss/locked_vm consistently with mapped pages.  Every
 *   mutation invalidates mm->seg_index, which is rebuilt lazily.
 * - pgdir belongs to the mm. Page-table writers must pair permission or mapping
 *   changes with the TLB flush required for the affected address space before
 *   user mode can observe stale translations.
 * - mm->lock is the intended serialization point for mapping list and
 *   page-table mutations. Some current paths still rely on single-threaded execution or
 *   narrower local locking; threaded user address spaces and SMP require those
 *   paths to be converted before broadening use.
 *
 * MM_LOCK_MODEL:
 * - Writers must hold mm->lock for mapping list mutations and user page-table
 *   mutations: mm_mmap/mm_mmap_file, mm_munmap, mm_mprotect, mm_mremap,
 *   mm_brk shrink, mm_fork COW setup, demand fault installs, COW fault installs,
 *   huge-page demotion, exec replacement, and exit teardown.
 * - ONE exception to the clause above, and it is narrow: the status fast path
 *   (mm_fault_from_status(), P5) installs a PTE for a pre-provisioned anonymous
 *   entry WITHOUT mm->lock, holding only the cursor.  It is the only PTE-write
 *   path in the tree that does not take mm->lock, which is the point of it: it
 *   consults no mapping, so there is nothing there for the lock to protect.
 *   Every other fault install -- COW, file, and any anon range not
 *   pre-provisioned -- still runs under mm->lock.  The exclusion it depends on is
 *   per-address exclusion against unmap (mm_pt_node_lock in pt_unmap_leaf /
 *   pt_unmap), not the absence of mm->lock.
 * - Read-only mapping walks may run without mm->lock only when the caller owns the
 *   task/mm exclusively or when the walk cannot race with mmap writers. Shared
 *   address-space readers need either mm->lock, or a held reference to the
 *   mapping (mm_seg_get) for as long as they use it.
 * - RSS accounting (mm_struct_t.rss_atomic) is the one mapping statistic that
 *   does NOT require mm->lock: the OOM victim selector reads it under
 *   proc_lock, which does not exclude the fault/COW/unmap writers. Use the
 *   mm_rss_* helpers, never a direct field access. The saturating subtract is
 *   a compare-exchange loop, not fetch_sub plus a clamp.
 * - Page-table writers must publish the new PTE before dropping the object/page
 *   reference it replaces and must flush the affected TLB range before returning
 *   to user mode. Permission relax/tighten, unmap, demote, demand fault, file
 *   mmap, COW, and dirty-bit updates are all page-table writes.
 * - File mappings own one fd and vnode reference. Shared file faults map the
 *   canonical page-cache page directly. Read-only private faults do the same
 *   and retain a mapping pin; if permission is later made writable, the first
 *   store copies to an anonymous page. Dirty PTEs are synced to the page cache
 *   before fsync/msync writeback so shared mmap writes are visible through
 *   read(), fsync(), and fork-shared cloning.
 * - OOM/reclaim must not free frames still reachable from task->mm, mapping lists,
 *   page tables, page cache refs, VMO refs, or Native handles. Reclaim may only
 *   choose unpinned cache/slab objects or kill a task and let normal exit/mm
 *   teardown release memory.
 */
typedef struct mm_struct {
    spinlock_t lock;
    /*
     * Serializes PTE invalidation through the remote shootdown and deferred
     * reference-drop phase.  A spinlock cannot cover that interval: remote
     * CPUs can be spinning on mm->lock with interrupts disabled and must be
     * allowed to leave that critical section before servicing the TLB IPI.
     */
    mutex_t tlb_lock;
    uint32_t active_cpus;    /* CPUs whose hardware context currently uses mm */
    uint32_t pt_readers;     /* cursors currently traversing this address space */
    uint32_t arch_asid;      /* nonzero tagged user address-space id */
    uint8_t tlb_pending;     /* transaction cleared/replaced at least one PTE */
    uint8_t _pad_tlb[3];
    uint64_t tlb_generation;
    uint64_t tlb_cpu_generation[CONFIG_NR_CPUS];
    vaddr_t tlb_start;
    vaddr_t tlb_end;
    /* The address space.  Ordered by start, non-overlapping; one record per
     * mmap, whatever backs it. */
    mm_seg_t *mmap;

    /* Ordered index over the list above: a cached, sorted copy so that
     * mm_seg_find() can binary-search instead of walking.
     *
     * Its entries are OWNED references (mm_seg_get), not borrowed pointers.
     * They could be borrowed only while the list owns every record for the
     * whole time the index is valid, and that is not true: unannotating a
     * page-table node entry puts a reference without invalidating this index,
     * so a borrowed entry could be freed underneath a later binary search.
     * Owning them costs one refcount per entry per rebuild and removes that
     * class of bug entirely.
     *
     * Rebuilt lazily (state 0=dirty, 1=valid, 2=overflow) and invalidated from
     * the same mutators that change the list, so it never disagrees about
     * whether it is current. */
    mm_seg_t *seg_index[MM_SEG_INDEX_CAPACITY];
    uint16_t seg_index_count;
    uint8_t seg_index_state; /* 0=dirty, 1=valid, 2=capacity overflow */
    uint8_t _pad_seg_index;
    mm_seg_t *deferred_vma;   /* released only after the last holder drops it */
    /*
     * Protects deferred_vma only.  Deliberately separate from mm->lock so a
     * lock-free page fault can drop its mapping reference (and therefore defer a
     * free) without taking the address-space lock it just escaped.  Held for
     * a list push/pop only -- never across vma_release(), which can run
     * blocking I/O.
     */
    spinlock_t vma_ref_lock;
    mm_tlb_hold_t *tlb_holds; /* released only after remote TLB shootdown */
    /* PT pages detached from the tree, awaiting a grace period.  Deliberately
     * NOT the tlb_holds list: a PT page is unreachable through the page table,
     * which is unrelated to "which addresses did this TLB transaction dirty".
     * Coupling them would force every teardown call site to open a transaction
     * and hold mm->lock, and several of them do neither. */
    mm_pt_retire_t *pt_retire;
    spinlock_t pt_retire_lock;
    pt_root_t *pgdir;
    vaddr_t    brk;
    vaddr_t    start_brk;
    vaddr_t    mmap_base;
    vaddr_t    stack_top;
    vaddr_t    stack_bottom;
    size_t     total_vm;
    /* Resident set size in pages.  Atomic: the OOM victim selector reads it
     * under proc_lock (kernel/mm/cg_mem.c) while fault/COW/unmap paths update
     * it under mm->lock, and once the status fault path leaves mm->lock the
     * two locks stop excluding each other.  The field name says "atomic" so
     * that a plain `->rss` access cannot compile -- see mm_rss_* below. */
    size_t     rss_atomic;
    size_t     locked_vm;
    uint32_t   def_flags;
    uint8_t    membarrier_registered; /* MEMBARRIER_CMD_REGISTER_* state */
    uint8_t    _pad_membarrier[3];
    uint8_t    has_vdso;   /* vDSO/vvar fixed mappings present (mm/vdso.h) */
    uint8_t    _pad_vdso[3];
    refcount_t refcount;
#ifdef CONFIG_NOMMU
    void      *nommu_allocs[NOMMU_ALLOC_MAX];
    size_t     nommu_alloc_sizes[NOMMU_ALLOC_MAX];
    uint8_t    nommu_alloc_types[NOMMU_ALLOC_MAX];
    int        num_nommu_allocs;
#endif
} mm_struct_t;

mm_struct_t *mm_create(void);
mm_struct_t *mm_get(mm_struct_t *mm);
void         mm_destroy(mm_struct_t *mm);
mm_struct_t *mm_fork(mm_struct_t *parent_mm);
void         mm_arch_context_init(mm_struct_t *mm);
uint64_t     mm_address_space_token(const mm_struct_t *mm);

mm_seg_t *mm_seg_find(mm_struct_t *mm, vaddr_t addr);

/* The same question asked of the segment index: which mapping covers `addr`?
 * Returns a BORROWED pointer -- the index owns the reference -- valid until
 * the index is invalidated.  Caller holds mm->lock, exactly as for
 * mm_seg_find(). */
struct mm_seg *mm_seg_find(mm_struct_t *mm, vaddr_t addr);

/* Mark the index stale without touching refcounts -- safe on every mapping
 * mutation, and the reason it is separate from mm_seg_index_clear(). */
void mm_seg_index_invalidate(mm_struct_t *mm);

/* Drop the index's owned references.  Called when the address space is torn
 * down; the invalidation path does not, because it cannot allocate. */
void mm_seg_index_clear(mm_struct_t *mm);
/*
 * Mapping lifetime (MM_AS_VMA_REFCOUNT, see the comment block in mm/vma.c).
 * One refcount covers three holders: the address-space list, the ordered
 * index, and the page-table node entries.  A page fault that reads a mapping
 * with mm->lock released must vma_get() first and vma_put() when done.
 *
 * The last holder does not free inline.  It queues the record on mm->deferred_vma
 * holding a reference of its own, and the flush point runs the release --
 * which can block on I/O, via vfs_close -- and only then drops the queue's
 * reference.  So no page-fault, index-rebuild or cursor path ever runs release
 * code, and no record on the queue can be freed by a later put underneath it.
 */
void vma_get(mm_seg_t *vma);
void vma_put(mm_struct_t *mm, mm_seg_t *vma);
void mm_vma_defer(mm_struct_t *mm, mm_seg_t *vma);
void mm_vma_flush_deferred(mm_struct_t *mm);
mm_seg_t *vma_try_merge(mm_struct_t *mm, mm_seg_t *vma);

struct vmo;
/* VMO-backed range export: returns a referenced VMO when [addr, addr+len)
 * is fully covered by a single VM_VMO VMA; prot_out receives the mapping's
 * protection bits.  Returns NULL when the range does not qualify. */
struct vmo *mm_lookup_vmo_region(mm_struct_t *mm, vaddr_t addr, size_t len,
                                 uint32_t *prot_out);
/* MADV_DONTNEED/MADV_FREE page discard over [addr, addr+len). */
int  mm_madvise_dontneed(mm_struct_t *mm, vaddr_t addr, size_t len);
/* Toggle the mlock-style VMA flag over [start, end) under mm->lock. */
int  mm_vma_set_lock(mm_struct_t *mm, vaddr_t start, vaddr_t end, int on);

vaddr_t    mm_find_gap(mm_struct_t *mm, vaddr_t hint, size_t len);
void       mm_insert_vma(mm_struct_t *mm, mm_seg_t *newv);
int        mm_split_vma_at(mm_struct_t *mm, vaddr_t addr);

void mm_sync_shared_dirty_for_vnode(struct vnode *vn);

#ifdef CONFIG_NOMMU
void mm_track_nommu_alloc(mm_struct_t *mm, void *ptr, size_t size, uint8_t type);
void mm_untrack_nommu_alloc(mm_struct_t *mm, void *ptr);
#endif

/* mmap helpers return either a user address or a negative errno encoded in
 * vaddr_t.  Convert through intptr_t so 32-bit errors remain negative. */
static inline int mm_addr_is_error(vaddr_t addr)
{
    return (intptr_t)addr < 0;
}

static inline int mm_addr_error(vaddr_t addr)
{
    return (int)(intptr_t)addr;
}

/* RSS accounting.  See the rss_atomic field comment for why these are atomic
 * rather than mm->lock protected.
 *
 * The saturating subtract is a compare-exchange loop and must not become a
 * fetch_sub followed by a clamp: with two concurrent subtracts the loser's
 * already-computed small value can be committed after the winner's decrement,
 * which is an underflow window that does not exist in the sequential version.
 * An underflowed rss feeds cg_mem's victim score, so it makes the OOM killer
 * sacrifice the wrong process. */
static inline void mm_rss_add(mm_struct_t *mm, size_t pages)
{
    if (mm)
        __atomic_fetch_add(&mm->rss_atomic, pages, __ATOMIC_RELAXED);
}

static inline void mm_rss_sub_clamped(mm_struct_t *mm, size_t pages)
{
    if (!mm)
        return;
    size_t cur = __atomic_load_n(&mm->rss_atomic, __ATOMIC_RELAXED);
    for (;;) {
        size_t next = cur > pages ? cur - pages : 0;
        if (__atomic_compare_exchange_n(&mm->rss_atomic, &cur, next, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

static inline void mm_rss_set(mm_struct_t *mm, size_t v)
{
    if (mm)
        __atomic_store_n(&mm->rss_atomic, v, __ATOMIC_RELEASE);
}

/* Acquire: the OOM victim selector reads this under proc_lock while other
 * state it weighs alongside rss is published under mm->lock. */
static inline size_t mm_rss_get(mm_struct_t *mm)
{
    return mm ? __atomic_load_n(&mm->rss_atomic, __ATOMIC_ACQUIRE) : 0;
}

/* Locked variants: caller must hold mm->lock.  Used by the public wrappers
 * and by internal call chains (mremap etc.) that already hold the lock. */
vaddr_t mm_mmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                       int prot, int flags);
vaddr_t mm_mmap_file_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                            int prot, int flags, int file_fd,
                            uint64_t file_offset);
vaddr_t mm_mmap_vmo_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                           int prot, int flags, struct vmo *vmo,
                           uint64_t vmo_offset);
int mm_munmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len);
/* A mapping record.  Created with one reference the creator owns; the index
 * and every annotation walk take their own with mm_seg_get(). */
mm_seg_t *mm_seg_new(void);
/* Naming a mapping in the page-table node-entry index, and un-naming it.  All
 * three are no-ops without page-table ops.
 *
 * The extent is a parameter rather than read off the mapping because the
 * callers that change a mapping's bounds have already changed them by the time
 * they get here, and the entries must be told both what the mapping named
 * before and what it names after. */
void mm_mmap_seg_annotate(mm_struct_t *mm, mm_seg_t *m, vaddr_t start,
                          vaddr_t end);
void mm_mmap_seg_retire(mm_struct_t *mm, mm_seg_t *m, vaddr_t start,
                        vaddr_t end);
/* Re-apply a mapping to the page-table path.  mmap creates no path, so without
 * this the annotation lands on nothing; call it once a fault has built part of
 * the path. */
void mm_mmap_seg_label(mm_struct_t *mm, mm_seg_t *m);
vaddr_t mm_brk_locked(mm_struct_t *mm, vaddr_t newbrk);
int mm_mprotect_locked(mm_struct_t *mm, vaddr_t addr, size_t len, int prot);
int mm_mremap_locked(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
                     size_t new_size, int flags, vaddr_t new_addr,
                     vaddr_t *out_addr);
vaddr_t mm_mmap(mm_struct_t *mm, vaddr_t addr, size_t len,
                int prot, int flags);
vaddr_t mm_mmap_file(mm_struct_t *mm, vaddr_t addr, size_t len,
                     int prot, int flags, int file_fd, uint64_t file_offset);
vaddr_t mm_mmap_vmo(mm_struct_t *mm, vaddr_t addr, size_t len,
                    int prot, int flags, struct vmo *vmo, uint64_t vmo_offset);
int     mm_munmap(mm_struct_t *mm, vaddr_t addr, size_t len);
vaddr_t mm_brk(mm_struct_t *mm, vaddr_t newbrk);
int     mm_mprotect(mm_struct_t *mm, vaddr_t addr, size_t len, int prot);
int     mm_mremap(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
                  size_t new_size, int flags, vaddr_t new_addr,
                  vaddr_t *out_addr);
int     mm_mseal(mm_struct_t *mm, vaddr_t addr, size_t len);
/* Non-mutating check: returns 1 if any mapping overlapping [addr, addr+len) is
 * sealed.  Used by ABI syscalls whose Linux contract requires -EPERM when a
 * covered mapping is sealed (madvise DONTNEED/FREE/REMOVE, etc.). */
int     mm_mseal_range_is_sealed(mm_struct_t *mm, vaddr_t addr, size_t len);
/* Locked variant of mm_mseal_range_is_sealed; caller holds mm->lock. */
int     mm_mseal_range_is_sealed_locked(mm_struct_t *mm, vaddr_t addr,
                                        size_t len);
int     mm_demote_huge_page(mm_struct_t *mm, vaddr_t addr);

/*
 * TLB invalidation transaction.  Public mapping writers take tlb_lock before
 * mm->lock, clear/replace PTEs while holding mm->lock, then call finish after
 * dropping mm->lock.  A frame/page hold keeps the old backing object alive
 * until every online CPU has acknowledged the invalidation.
 */
void mm_tlb_invalidate_begin(mm_struct_t *mm);
void mm_tlb_invalidate_finish(mm_struct_t *mm);
void mm_tlb_shootdown_page(mm_struct_t *mm, vaddr_t addr);
void mm_tlb_note_change(mm_struct_t *mm, vaddr_t addr, size_t size);
int  mm_tlb_hold_frame(mm_struct_t *mm, pfn_t pfn);
int  mm_pt_hold_table(mm_struct_t *mm, pfn_t pfn, int level);
int  mm_tlb_hold_page(mm_struct_t *mm, struct page_cache_page *page);
void mm_context_enter(mm_struct_t *mm, unsigned cpu);
void mm_context_leave(mm_struct_t *mm, unsigned cpu);

pte_t mm_prot_to_pte_flags(int prot);
int   mm_pte_flags_to_prot(pte_t pte_flags);
pte_t mm_vm_flags_to_pte_flags(uint64_t vm_flags);
uint64_t mm_pte_flags_to_vm_flags(pte_t pte_flags);
pte_t mm_user_stack_pte_flags(void);
pte_t mm_user_brk_pte_flags(void);
int   mm_pte_flags_allow_access(pte_t pte_flags);
pte_t mm_pte_flags_apply_prot(pte_t old_flags, pte_t prot_flags);
pte_t mm_pte_flags_make_writable_dirty(pte_t pte_flags);

/* User-space W^X policy (mm/wx.c): parses the a20.wx= cmdline and filters
 * W|X protection bits out of a requested prot. */
void mm_wx_policy_init(void);
int  mm_wx_filter_prot(int prot, const char *ctx);

/* User-space ASLR (mm/aslr.c): per-process random layout offsets applied at
 * exec time. */
vaddr_t mm_aslr_mmap_base(void);
vaddr_t mm_aslr_stack_offset(void);
vaddr_t mm_aslr_brk_offset(vaddr_t brk_base);

#endif
