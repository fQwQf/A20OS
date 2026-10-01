#ifndef _MM_PT_H
#define _MM_PT_H

#include "core/types.h"
#include "core/arch.h"
#include "core/lock.h"

/*
 * MM_AS_MODEL — single-level address-space model (CortenMM-style).
 *
 * The traditional design keeps TWO representations of a mapping and must
 * keep them consistent: a software-level interval structure (mm->mmap, the
 * VMA list, guarded by mm->lock) and the hardware page tables.  Every
 * operation has to reconcile both, which is where the locking complexity and
 * the concurrency bugs come from.
 *
 * This model removes the software-level abstraction as the SOURCE OF TRUTH
 * for mapping state.  The authoritative per-virtual-page state is the
 * per-PTE metadata array attached to the page-table page that covers it.
 * A transactional cursor is the only way to program the MMU, and the
 * concurrency control is expressed directly on page-table-page locks, which
 * lets transactions over disjoint ranges proceed in parallel.
 *
 * MM_AS_CURSOR_ONLY_ENTRY
 * -----------------------
 * Invariants that hold for every code path in kernel/mm:
 *
 *  1. Every read or write of a user PTE happens inside an mm_cursor_t, i.e.
 *     between mm_addrspace_lock() and mm_cursor_unlock().  No code path may
 *     walk a user page table with pt_walk()/pt_lookup_leaf() and then keep
 *     the resulting pte_t* across a lock acquisition, a blocking call, or a
 *     return to userspace.  pt_walk()/pt_lookup_leaf() remain as
 *     non-authoritative helpers for teardown, auditing and /proc reporting,
 *     where no mutation follows.
 *  2. Every page-table WRITE allocates intermediate page-table pages only
 *     through the cursor, and every such allocation installs a metadata
 *     block (pt_meta_t) so the covering node always has a lock.
 *  3. The page-table-page lock is the unit of writer exclusion.  A cursor
 *     acquires locks in page-table preorder and releases them in exactly
 *     the reverse order.
 *  4. The kernel half of a page table is SHARED between every address space
 *     (pt_map_kernel() copies the boot PTE words).  Those nodes must never
 *     be locked, mutated, or freed through a cursor: mm_addrspace_lock()
 *     refuses any range that intersects them.  Kernel mappings are
 *     programmed by the boot path, never by a cursor.
 *  5. The VMA list (mm->mmap) is NOT the source of truth for mapping state.
 *     During the transition it is a non-authoritative interval index used
 *     only for: interval policy that is genuinely range-shaped (brk bounds,
 *     mseal sealing, mlock accounting), backing-store ownership
 *     (file_fd/vmo references), and /proc/{maps,smaps} reporting.
 *     mm_pt_audit_addrspace() proves the two representations agree; it is
 *     the only sanctioned way to treat the VMA list as derived state.
 */

/* ------------------------------------------------------------------ *
 * Per-virtual-page status
 * ------------------------------------------------------------------ *
 * A per-PTE metadata entry packs into one byte:
 *
 *   bits 7..4  status class
 *   bit  3     COW "shared" bit (multiple mappings may reference the frame)
 *   bits 2..0  access permission R/W/X
 *
 * The physical frame of a mapped page is NOT duplicated here: it is already
 * in the PTE, and its reference count lives in pfa.meta[].refcount.  The
 * metadata only carries what the MMU cannot express, which is exactly the
 * information a page fault needs to decide what to do.
 *
 * Storing this in the PTE's software-usable bits is not an option on A20OS:
 * riscv32 and arm32 have no free software bits at their root levels, and
 * loongarch64 aliases PTE_R/W/X onto the LA_PTE memory-attribute field.  The
 * existing PTE_SWAP already steals a hardware-meaningful bit per
 * architecture, which is precisely the hazard a general status encoding
 * would multiply.
 */
#define MM_ST_INVALID        0u  /* no mapping, no backing */
#define MM_ST_ANON_VIRT      1u  /* virtually allocated, not yet backed */
#define MM_ST_ANON_MAPPED    2u  /* mapped to a private frame */
#define MM_ST_ANON_SHARED    3u  /* mapped to a frame shared via fork */
#define MM_ST_FILE_PRIVATE   4u  /* file-backed, private copy on write */
#define MM_ST_FILE_SHARED    5u  /* file-backed, canonical cache frame */
#define MM_ST_VMO            6u  /* backed by a native-ABI VMO */
#define MM_ST_SWAPPED        7u  /* contents live on a swap device */
#define MM_ST_PT_NODE        8u  /* this entry is an intermediate PT page */
#define MM_ST_CLASS_MAX      9u

#define MM_ST_CLS_BYTE(c)    ((uint8_t)(((c) & 0xFu) << 4))
#define MM_ST_GET_CLASS(b)   ((uint8_t)(((b) >> 4) & 0xFu))
#define MM_ST_COW_BIT        0x08u
#define MM_ST_PROT_R         0x01u
#define MM_ST_PROT_W         0x02u
#define MM_ST_PROT_X         0x04u
#define MM_ST_PROT_MASK      0x07u
#define MM_ST_PROT_ALL       (MM_ST_PROT_R | MM_ST_PROT_W | MM_ST_PROT_X)

#define MM_ST_IS_MAPPED(c)                                            \
    ((c) == MM_ST_ANON_MAPPED || (c) == MM_ST_ANON_SHARED ||           \
     (c) == MM_ST_FILE_SHARED)

/* ------------------------------------------------------------------ *
 * Page-table page descriptor
 * ------------------------------------------------------------------ *
 * pt_meta_t hangs off the physical frame that backs a page-table page.  It
 * is reached through pfa.meta[], which is already a contiguous, PPN-indexed,
 * permanently direct-mapped array, so the descriptor lookup is one indexed
 * load (see mm_pt_meta()) rather than a hash or slab probe.  The ADV locking
 * protocol needs the descriptor during its LOCKLESS traversal -- to read
 * `stale` and to take the covering node's lock -- which is the hottest path
 * in the system, so the lookup must be cheap.
 *
 * The pointer is stored in the frame metadata union that is otherwise only
 * live while a frame sits on a buddy free list, so this costs no additional
 * memory per frame.  The block itself is allocated on demand, one per
 * page-table page, and freed together with that page-table page.
 */
#ifndef MM_PT_META_ENTRIES
# define MM_PT_META_ENTRIES ARCH_PT_LEVEL_ENTRIES(0)
#endif

typedef struct pt_meta {
    /* MCS lock body.  A queued lock rather than a flat spinlock: a cursor
     * may hold a hierarchy of up to ARCH_PT_LEVELS nodes covering many
     * descendants, and FIFO ordering avoids the convoy/inversion pathology
     * a flat spinlock exhibits there. */
    volatile uintptr_t lock;
    /* Holder-private MCS node, stashed here while the lock is held so it can
     * be recovered at unlock.  Touched only by the holder. */
    uintptr_t         node;
    uint16_t          nr_present;   /* present or PT-node entries */
    uint8_t           level;        /* page-table depth of this page */
    uint8_t           stale;        /* detached from parent; subtree poisoned */
    uint8_t           cls[MM_PT_META_ENTRIES];
    uint8_t           cow[(MM_PT_META_ENTRIES + 7) / 8];
    /* Safety semantics that the status byte has no room for: all 8 bits are
     * allocated (4 class + COW + 3 prot) and shared-ness already lives in the
     * class field, so these ride alongside `cow` as a bitmap rather than
     * widening cls[] to 16 bits per entry. */
    uint8_t           safe[(MM_PT_META_ENTRIES + 7) / 8];
} pt_meta_t;

/*
 * Per-entry safety bits.  These exist so the per-PTE status can become the
 * authority that replaces the VMA in the fault path (docs 10.7): without them
 * a status-driven fault would bypass userfaultfd and fault-around's safety
 * gate, because the status byte cannot express either.
 */
/* A userfaultfd range covers this entry: a fault here must be parked for the
 * handler, never satisfied by fabricating a zero page. */
#define MM_SAFE_UFFD     (1u << 0)
/* Multi-page fault-around must not cover this entry (sealed VMA, or a class
 * where speculative allocation would change semantics). */
#define MM_SAFE_NO_FA    (1u << 1)
#define MM_SAFE_MASK     (MM_SAFE_UFFD | MM_SAFE_NO_FA)

struct mm_struct;

/* A transaction over one virtual address range.  Every field is private;
 * callers use the accessors below.
 *
 * The descent is cached at lock time.  A cursor must not re-walk the page
 * table per operation: that would make a transaction cost more than the bare
 * pt_walk() it replaces, and -- once mm->lock no longer serialises mutators
 * in P5 -- a re-descent could observe a path that a concurrent unmap has
 * already unlinked.  Caching is what makes the cursor a single-descent
 * primitive.
 *
 * Locking, precisely (this used to claim a preorder DFS over the whole
 * subtree, which the code never did):
 * - The cursor holds the COVERING node's lock for its whole lifetime.  That
 *   alone does NOT exclude a peer whose covering node is an ancestor or
 *   descendant: a wide cursor (covering level 2) and a single-page cursor
 *   (covering level 0) inside it would hold disjoint locks while writing the
 *   same leaf PTE and the same pt_meta_t.cls[] byte.
 * - Every leaf the cursor actually touches is therefore locked individually
 *   and released at the end of that one operation (cursor_leaf_slot /
 *   cursor_leaf_unlock).  Since every write targets a single leaf entry, that
 *   is what makes two cursors conflict exactly when they touch the same leaf.
 * - A wide cursor visits many leaves and cannot hold them all at once -- the
 *   per-CPU held[] stack has PT_MCS_POOL_SLOTS entries -- hence per-operation
 *   rather than per-transaction acquisition.
 * - Intermediate nodes are installed under the parent's lock and released
 *   immediately.  A single aligned PTE store publishes the new child, so a
 *   concurrent reader sees either "absent" or a valid child, never a torn one.
 *   The read-side bracket (mm->pt_readers) keeps an already-cached page from
 *   being recycled underneath the descent; `stale` makes a cursor that lost a
 *   race abandon the node and re-descend.
 */
#define MM_CURSOR_PATH_MAX (ARCH_PT_ROOT_LEVEL + 1)

typedef struct mm_cursor {
    struct mm_struct *mm;
    vaddr_t           start;
    vaddr_t           end;
    int               locked;
    int               guard_level;  /* level of the covering node */
    pte_t            *path[MM_CURSOR_PATH_MAX];
    /* Per-CPU page-table lock depth when the cursor opened.  Unlock pops
     * back to this depth, so a preorder DFS of any width unwinds in reverse
     * without the cursor having to store one entry per locked node. */
    int               lock_base_depth;
    int               in_read_side;   /* holds mm->pt_readers */
    /* Leaf table locked by the in-flight cursor operation, released by
     * cursor_leaf_unlock().  Not part of the unwind: exactly one is held at a
     * time, for the duration of a single map/unmap/mark/query. */
    struct pt_meta   *leaf_meta;
} mm_cursor_t;

/* The report is plain data so NOMMU builds can still reference the type and
 * report zeros; the walkers themselves are page-table builds only. */
/* Deferred page-table page reclamation (P4).
 *
 * A cursor's descent caches physical page-table pointers, so a concurrent
 * unmap that frees such a page would let the cursor keep operating on
 * recycled memory.  Detach therefore does not free: it marks the subtree
 * stale, unlinks it, and queues the page with the address space's TLB
 * invalidation barrier.  The page is released only once the shootdown has
 * completed AND no cursor remains in a read-side critical section, which
 * together prove no traversal can still reach it. */
void mm_pt_read_enter(struct mm_struct *mm);


void mm_pt_read_exit(struct mm_struct *mm);

/* Queue a detached page-table page.  level is the page-table depth, used to
 * recover the buddy order at drain time.  Caller holds the covering node's
 * lock (or mm->lock) and must already have unlinked the page. */
int  mm_pt_defer_free(struct mm_struct *mm, pte_t *table, int level);

/* Mark a detached subtree stale so a cursor that acquires it retries. */
void mm_pt_mark_stale_recursive(pte_t *table, int level);

/* Per-entry safety bits (see MM_SAFE_* above).  Set/clear/test one bit at a
 * time; every mutation of a slot must go through these so the bits cannot
 * outlive the class they describe. */
int  mm_pt_safe_set(pte_t *table, int level, int idx, unsigned flags);
int  mm_pt_safe_clear(pte_t *table, int level, int idx, unsigned flags);
/* Clear `flags` on the single page `va`.  Used instead of a range-wide clear
 * where a mark may be co-owned (see MM_SAFE_UFFD). */
int  mm_pt_safe_clear_page(struct mm_struct *mm, vaddr_t va, unsigned flags);

/* Eager-provisioning cap, in pages (0 disables).  Writable at runtime via
 * /proc/a20/anonprov so a benchmark can alternate the two settings inside a
 * single boot instead of comparing two separately booted kernels. */
uint32_t mm_pt_anon_prov_max(void);
int      mm_pt_set_anon_prov_max(uint32_t pages);
int  mm_pt_safe_test(pte_t *table, int level, int idx, unsigned flags);
int  mm_cursor_safe_test(mm_cursor_t *cur, vaddr_t addr, unsigned flags);
int  mm_pt_set_safe_range(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                          unsigned flags, int set);

/* Grace-period reclamation for detached PT pages; see the comment above
 * mm_pt_retire_drain().  Callable from any context -- needs neither mm->lock
 * nor a TLB transaction. */
void mm_pt_retire_table(struct mm_struct *mm, pte_t *table, int level);
void mm_pt_retire_drain(struct mm_struct *mm);

/* ---- invariant auditing ---- */
typedef struct mm_pt_audit_report {
    uint64_t pt_pages;
    uint64_t entries;
    uint64_t missing_meta;   /* PT page live with no metadata block */
    uint64_t present_mismatch;/* PTE present but class says otherwise */
    uint64_t absent_mismatch;/* class non-Invalid but no PTE */
    uint64_t prot_mismatch;  /* permission bits disagree with the PTE */
    uint64_t cow_mismatch;   /* COW bit disagrees with PTE_COW */
    uint64_t vma_mismatch;   /* VMA coverage disagrees with the status */
    uint64_t safe_mismatch;  /* MM_SAFE_NO_FA disagrees with VM_SEALED */
    /* Not an error: how many leaves carry MM_AS_ANON_VIRT, i.e. are reserved
     * but not yet backed.  This is the on-demand paging state the paper
     * relies on, so it is counted to make it observable rather than inferred
     * from the absence of mismatches. */
    uint64_t anon_virt;
} mm_pt_audit_report_t;

static inline uint64_t mm_pt_audit_errors(const mm_pt_audit_report_t *r)
{
    return r->missing_meta + r->present_mismatch + r->absent_mismatch +
           r->prot_mismatch + r->cow_mismatch + r->vma_mismatch +
           r->safe_mismatch;
}


#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)

/* Descriptor access.  table is a direct-mapped pointer to a page-table page. */
pt_meta_t *mm_pt_meta(pte_t *table);
static inline int mm_pt_meta_level(const pt_meta_t *m) { return m->level; }
static inline int mm_pt_meta_stale(const pt_meta_t *m)  { return m->stale; }
void mm_pt_meta_set_stale(pt_meta_t *m, int stale);

/* The table that owns the leaf slot for addr, i.e. the parent of the leaf.
 * This is the table whose metadata array describes that virtual page -- NOT
 * the table pt_walk() returns, which is the leaf table itself. */
pte_t *mm_pt_leaf_table(pt_root_t *pgdir, vaddr_t addr);

/* Map hardware PTE permission bits onto the status byte's R/W/X field. */
uint8_t mm_pt_prot_bits(pte_t flags);

/* One-time core init: pre-size the per-CPU MCS node pools so the page-table
 * lock path never allocates.  Call once from mm_init(). */
void mm_pt_core_init(void);

/* Install / tear down the metadata block for a freshly allocated PT page. */
int  mm_pt_node_init(pte_t *table, int level);
void mm_pt_node_fini(pte_t *table);

/* Per-PTE metadata maintenance.  mm_pt_note_present() and
 * mm_pt_note_absent() bracket every PTE write; the level-0 helpers are the
 * leaf forms.  The AUDIT-only entry point exists so the auditor can compare
 * the two representations without going through a cursor. */
void mm_pt_note_present(pte_t *table, int level, int idx, uint8_t cls_byte);
void mm_pt_note_absent(pte_t *table, int level, int idx);
uint8_t mm_pt_peek(pte_t *table, int level, int idx);
int mm_pt_cow(pte_t *table, int level, int idx);
void mm_pt_set_cow(pte_t *table, int level, int idx, int on);

/* Copy a page-table page's metadata to a freshly cloned page. */
int mm_pt_meta_clone(pte_t *dst_table, pte_t *src_table, int level);

/* True when [start,end) lies entirely in the user half, i.e. it is safe to
 * lock and mutate those page-table nodes.  The kernel half is shared with
 * every other address space and must never be touched by a cursor. */
int mm_pt_range_is_user(vaddr_t start, vaddr_t end);

/* ---- transactional interface ---- */
int  mm_addrspace_lock(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                       mm_cursor_t *cur);
void mm_cursor_unlock(mm_cursor_t *cur);

/* Returns 1 and fills *cls_out when a leaf covers addr.  Returns 0 when the
 * address is not mapped.  *pa_out receives the physical address when the
 * leaf is present. */
int mm_cursor_query(mm_cursor_t *cur, vaddr_t addr, uint8_t *cls_out,
                    paddr_t *pa_out);
int mm_cursor_map(mm_cursor_t *cur, vaddr_t addr, paddr_t pa, pte_t flags,
                  uint8_t cls);

/* Install a mapping and report the displaced frame in *old_pa_out WITHOUT
 * releasing it.  The COW path must hold the old reference until after the
 * remote TLB shootdown, and its rc==1 branch already runs under pfa.lock, so
 * a frame_put() inside a combined map would deadlock. */
int mm_cursor_replace(mm_cursor_t *cur, vaddr_t addr, paddr_t pa, pte_t flags,
                      uint8_t cls, paddr_t *old_pa_out);

int mm_cursor_unmap(mm_cursor_t *cur, vaddr_t addr);
int mm_cursor_mark(mm_cursor_t *cur, vaddr_t addr, uint8_t cls);
int mm_cursor_mark_prot(mm_cursor_t *cur, vaddr_t addr, uint8_t cls,
                        pte_t flags);
int mm_pt_refresh_absent_prot(pte_t *table, int idx, pte_t ptef);

/*
 * Eagerly provision an anonymous range: build the page-table path and mark
 * every leaf MM_ST_ANON_VIRT with its permissions, so a later fault on the
 * range can be served from per-PTE status alone instead of re-deriving the
 * mapping from a VMA.  This mirrors the paper's on-demand paging (SS4.3).
 *
 * Returns 0, or a negative errno.  A range already covered (e.g. by a
 * MAP_FIXED mapping over live pages) is left alone, and a mapping whose
 * permissions changed since provisioning is re-marked, so this stays correct
 * across mprotect.
 */
/* Eager per-PTE reservation for anonymous ranges, up to this many pages.
 * Larger mappings keep the VMA-based fault path: provisioning them would
 * cost one page-table descent per page, which is a poor trade for a range
 * that is likely to stay sparse. */
#define MM_ANON_PROVISION_MAX_PAGES 4096u

/*
 * Eager provisioning is OFF by default, and boots that way unless
 * a20.anonprov=<pages> is given.  Interleaved A/B (docs 10.14) showed it costs
 * ~14% on mmap -- 6/6 pairs, p~0.016 -- while buying nothing measurable,
 * because the only consumer of the status marks, the status-driven fault path,
 * has never executed (mm_fault_from_status is always 0, docs 10.6).  Paying
 * 14% for zero benefit is a net loss, so the default is off.
 *
 * MM_ANON_PROVISION_MAX_PAGES is the recommended value to pass when the
 * status fault path does become usable; the groundwork stays in place and is
 * one boot parameter away, and this default costs no functionality.
 */
#define MM_ANON_PROV_KEY   "a20.anonprov"
void mm_pt_anon_prov_init(void);

int mm_pt_provision_anon(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                         pte_t flags);

/* Walk an address space and prove metadata == page tables (and, when
 * check_vma is set, == VMA coverage).  Returns 0 when the address space is
 * fully consistent. */
int mm_pt_audit_addrspace(struct mm_struct *mm, int check_vma,
                          mm_pt_audit_report_t *out);

/* Audit every live address space, aggregating into *out.  Callers must not
 * hold proc_lock. */
int mm_pt_audit_all(mm_pt_audit_report_t *out);

#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */

#endif /* _MM_PT_H */
