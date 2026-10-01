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

/* Typical compiler processes have O(100) VMAs.  Keep a compact secondary
 * pointer index for binary-search lookup while retaining the linked list as
 * the ownership and mutation representation. */
/* rustc/LLVM address spaces routinely exceed the old 256-VMA cutoff; below
 * that the lookup degraded to a linear scan on every page fault.  1024 keeps
 * binary search for realistic compiler processes (8 KiB per mm_struct). */
#define MM_VMA_INDEX_CAPACITY 1024

#ifdef CONFIG_NOMMU
#define NOMMU_ALLOC_MAX    32
#define NOMMU_ALLOC_IMAGE  0
#define NOMMU_ALLOC_STACK  1
#define NOMMU_ALLOC_TLS    2
#endif

typedef struct vm_area {
    vaddr_t         start;
    vaddr_t         end;
    uint64_t        vm_flags;
    pte_t           pte_flags;
    uint32_t        vmar_cap;       /* Native VMAR capability (PROT bits) at creation */
    int             file_fd;
    int             sysv_shmid;
    uint64_t        file_offset;
    struct vmo     *vmo;
    uint64_t        vmo_offset;
    struct vnode   *file_vnode;     /* referenced vnode for every VM_FILE */
#ifdef CONFIG_NOMMU
    void           *nommu_alloc;     /* raw allocation backing this VMA */
#endif
    struct vm_area *prev;
    struct vm_area *next;
    /*
     * MM_AS_VMA_REFCOUNT:
     * The single-level model needs a page fault to read a VMA's fields with
     * mm->lock released -- that is what stops one address-space lock from
     * serialising every fault in the process.  Once the lock is gone, the VMA
     * pointer itself is no longer protected, so the fault takes a reference
     * and drops it when done.  The address-space list owns the reference
     * created at allocation; unlinking drops it, and the last holder (which
     * may be a fault running lock-free) is what defers the free.
     */
    refcount_t      refcount;
    /* Deferred-free linkage.  Separate from `next` so a VMA on the deferred
     * list is never confused with one on mm->mmap. */
    struct vm_area *deferred_next;
} vm_area_t;

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
 * MM_LOCK_MODEL below describes the two-level model this file is being
 * migrated away from, and still governs every mutator that has not yet been
 * converted.  The target model is specified in mm/pt.h; the short version:
 *
 *  - The authoritative per-virtual-page state is the per-PTE metadata array
 *    attached to the covering page-table page (pt_meta_t.cls[]), not a VMA.
 *    A VMA is an interval claim; the metadata is a per-page claim, and a page
 *    fault resolves from the latter.
 *  - All page-table programming goes through the transactional cursor
 *    (mm_addrspace_lock / mm_cursor_{query,map,mark,unmap}), whose unit of
 *    writer exclusion is the page-table-page lock.  Transactions over
 *    disjoint ranges do not serialize.
 *  - Status lives in a side array rather than in the PTE's software bits,
 *    because riscv32/arm32 have no free software bits at their root levels
 *    and loongarch64 aliases PTE_R/W/X onto its memory-attribute field.
 *  - The VMA list survives as a non-authoritative interval index for the
 *    things that are genuinely range-shaped (brk bounds, mseal, mlock
 *    accounting, /proc maps).  It is proved derived, not assumed, by
 *    mm_pt_audit_addrspace(), which runs over every live address space at
 *    shutdown.
 *
 * Migration status: the descriptor, the per-PTE metadata, the cursor and
 * the auditor are in place and the metadata is maintained at every page-table
 * write in kernel/mm/mm.c.  Page faults still resolve through mm_find_vma();
 * moving fault dispatch onto the status is the next step.  Until then the
 * two representations are both live and the auditor is what keeps them
 * honest, so do not treat the cursor as the only entry point yet.
 *
 * mm_struct_t lifetime and address-space invariants:
 * - refcount is shared by every task/thread that uses the same address space.
 *   A task_t may store mm == NULL only for kernel-only tasks or after teardown
 *   has detached it from user memory.
 * - mm_destroy() drops one reference. The final reference owns destruction of
 *   all VMAs, VMA-backed resources, user page-table leaves, and the user page
 *   table itself; callers must not keep VMA or pgdir pointers after dropping the
 *   last reference.
 * - mmap is an ordered VMA list owned by the mm. VMA insertion, removal, split,
 *   merge, and resource release must preserve non-overlap and must account
 *   total_vm/rss/locked_vm consistently with mapped pages.
 * - pgdir belongs to the mm. Page-table writers must pair permission or mapping
 *   changes with the TLB flush required for the affected address space before
 *   user mode can observe stale translations.
 * - mm->lock is the intended serialization point for VMA list and page-table
 *   mutations. Some current paths still rely on single-threaded execution or
 *   narrower local locking; threaded user address spaces and SMP require those
 *   paths to be converted before broadening use.
 *
 * MM_LOCK_MODEL:
 * - Writers must hold mm->lock for mmap list mutations and user page-table
 *   mutations: mm_mmap/mm_mmap_file, mm_munmap, mm_mprotect, mm_mremap,
 *   mm_brk shrink, mm_fork COW setup, demand fault installs, COW fault installs,
 *   huge-page demotion, exec replacement, and exit teardown.
 * - Read-only VMA walks may run without mm->lock only when the caller owns the
 *   task/mm exclusively or when the walk cannot race with mmap writers. Shared
 *   address-space readers need either mm->lock, a pinned VMA/page-cache object,
 *   or a future RCU-style VMA lifetime scheme.
 * - RSS accounting (mm_struct_t.rss_atomic) is the one mapping statistic that
 *   does NOT require mm->lock: the OOM victim selector reads it under
 *   proc_lock, which does not exclude the fault/COW/unmap writers. Use the
 *   mm_rss_* helpers, never a direct field access. The saturating subtract is
 *   a compare-exchange loop, not fetch_sub plus a clamp.
 * - Page-table writers must publish the new PTE before dropping the object/page
 *   reference it replaces and must flush the affected TLB range before returning
 *   to user mode. Permission relax/tighten, unmap, demote, demand fault, file
 *   mmap, COW, and dirty-bit updates are all page-table writes.
 * - File VMAs own one fd and vnode reference. Shared file faults map the
 *   canonical page-cache page directly. Read-only private faults do the same
 *   and retain a mapping pin; if permission is later made writable, the first
 *   store copies to an anonymous page. Dirty PTEs are synced to the page cache
 *   before fsync/msync writeback so shared mmap writes are visible through
 *   read(), fsync(), and fork-shared cloning.
 * - OOM/reclaim must not free frames still reachable from task->mm, VMA lists,
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
    vm_area_t *mmap;
    vm_area_t *vma_index[MM_VMA_INDEX_CAPACITY];
    uint16_t vma_index_count;
    uint8_t vma_index_state; /* 0=dirty, 1=valid, 2=capacity overflow */
    uint8_t _pad_vma_index;
    vm_area_t *deferred_vma;  /* released only after the last holder drops it */
    /*
     * Protects deferred_vma only.  Deliberately separate from mm->lock so a
     * lock-free page fault can drop its VMA reference (and therefore defer a
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

vm_area_t *mm_find_vma(mm_struct_t *mm, vaddr_t addr);
/*
 * VMA lifetime (MM_AS_VMA_REFCOUNT, see the comment block in mm/vma.c).
 * The address-space list owns the reference created at allocation; a page
 * fault that reads VMA fields with mm->lock released must vma_get() first and
 * vma_put() when done.  The last holder schedules the deferred free, which
 * never runs inline because vma_release() can block on I/O.
 */
void vma_get(vm_area_t *vma);
void vma_put(mm_struct_t *mm, vm_area_t *vma);
void mm_vma_defer(mm_struct_t *mm, vm_area_t *vma);
void mm_vma_flush_deferred(mm_struct_t *mm);
vm_area_t *vma_try_merge(mm_struct_t *mm, vm_area_t *vma);

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
void       mm_insert_vma(mm_struct_t *mm, vm_area_t *newv);
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
/* Non-mutating check: returns 1 if any VMA overlapping [addr, addr+len) is
 * sealed.  Used by ABI syscalls whose Linux contract requires -EPERM when a
 * covered VMA is sealed (madvise DONTNEED/FREE/REMOVE, etc.). */
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
