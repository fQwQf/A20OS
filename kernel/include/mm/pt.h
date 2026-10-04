#ifndef _MM_PT_H
#define _MM_PT_H

#include "core/types.h"
#include "core/arch.h"
#include "core/lock.h"
/* The mapping record lives here, written in terms of the VM_* bits; pt.h only
 * needs the type, and taking the definition from vm.h keeps one copy of it. */
#include "mm/vm.h"

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
 *
 *     MM_AS_CURSOR_ONLY_ENTRY_BYPASSES names the functions that break rule 1
 *     today, with what is actually holding them safe.  The list is not
 *     decoration: removing mm->lock from a fault path without accounting for it
 *     turns each entry into a concurrent unlocked RMW against cursor-locked
 *     faults on the same leaf.  That is not hypothetical -- it is what the
 *     Phase 3 attempt hit (see docs/roadmap/single-level-mm-model.md 11.7
 *     item 1).  A new bypass has to be added here, which is what makes it a
 *     decision rather than an oversight; check-mm-pt-lock-order asserts the list
 *     still matches the code.
 *
 *     MM_AS_CURSOR_ONLY_ENTRY_BYPASSES
 *       pt_unmap_leaf   mm.c   no cursor; every mutation under mm_pt_node_lock
 *       pt_unmap        mm.c   no cursor; every mutation under mm_pt_node_lock
 *
 *     These two do not satisfy rule 1 as written -- they are not cursors and
 *     hold no range-wide atomicity, which is why they stay listed instead of
 *     being quietly declared compliant.  What they do guarantee is the part
 *     rule 3 asks for: every write takes the lock of the PT page being written,
 *     the same page cursor_leaf_slot() locks, so a fault and an unmap of one
 *     address exclude each other.  The gate checks each listed function still
 *     brackets its writes with that lock, so dropping it fails the build.
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
 *
 * MM_AS_MODEL -- PTE_SWAP is THE ONE tolerated exception (2026-10-02)
 * ------------------------------------------------------------------ *
 * Unlike the status byte, the swapped bit is *not* moved into metadata:
 * pte_to_swp_entry() recovers a 44-bit payload on 64-bit architectures,
 * which does not fit the one-byte status and would require widening it to
 * 8 bytes per entry -- 4 KiB of overhead per PT page, prepaid on pages
 * that will never hold a swapped-out page.  The paper specifies no
 * replacement encoding, so removing PTE_SWAP is not "following the paper"
 * but an undesigned format change.  Decision and measurements:
 * docs/roadmap/single-level-mm-model.md (P7 record).
 *
 * The exception is arch-dependent, and on three of six architectures it is
 * larger than "one bit":
 *
 *   x86_64 / aarch64 / loongarch64   PTE_SWAP == PTE_LEAF (leaf marker)
 *   riscv64                          1UL << 9
 *   arm32                            1U << 7
 *   ppc64le                          0x2
 *
 * i.e. on the first group the encoding is !PTE_V && PTE_SWAP ==> swapped,
 * so the bit being reused is the most semantically loaded one in a leaf
 * PTE, not a spare software bit.
 *
 * The invariant is therefore narrowed, not dropped: PTE_SWAP is the ONLY
 * overloaded bit, and every architecture must define it explicitly.
 * check-mm-pt-lock-order asserts this per architecture.  The hazard worth
 * guarding against is not this bit's existence -- it being read as licence
 * to take one more.
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
    /* Backing-object inheritance, allocated only for page-table pages that
     * actually carry file/VMO node annotations (see mm_segtab_t).  NULL on
     * every PT page of a purely anonymous address space, so anonymous
     * workloads pay exactly nothing. */
    struct mm_segtab *segtab;
} pt_meta_t;

/* ------------------------------------------------------------------ *
 * Backing-object segments  (P6, see docs/roadmap/single-level-mm-model.md 12.6)
 * ------------------------------------------------------------------ *
 * The status byte records WHAT a page is (class + COW + prot) but cannot
 * record WHICH OBJECT backs it -- a vnode, an offset, a VMO.  Until fault
 * dispatch can name that object from the status alone, it has to ask the VMA,
 * and the VMA cannot be deleted.
 *
 * Those facts are per RANGE, not per page: one 3 GiB file mapping has one
 * offset sequence, not 786432 of them.  So they live in a segment shared by
 * every page-table page the mapping crosses.
 *
 * WHY THE INDEX IS PER NODE ENTRY, NOT PER LEAF
 * ----------------------------------------------
 * The obvious encoding -- a per-leaf segment index -- does not work, and the
 * reason is worth keeping because it is not obvious.  Writing a leaf entry
 * requires that leaf table to exist, so annotating an un-faulted range by
 * leaf would force every page-table page in that range to be materialised: a
 * 3 GiB mapping becomes 1536 leaf tables plus 1536 pt_meta_t.  That is
 * precisely the cost g_anon_prov_max exists to cap (measured: 512 cursor
 * round trips for a 2 MiB mapping already cost ~1.5x), and precisely why
 * eager provisioning ships off.
 *
 * Putting the index on the PARENT's entry for a child node instead makes the
 * annotation mean "this whole subtree is backed by segment S".  mmap writes
 * one index per node on the path -- three or four for a 3 GiB Sv39 mapping --
 * and a fault descent stops at the first annotated node.  Cost is O(nodes).
 * The four class bits currently spent on MM_ST_PT_NODE are free to carry it,
 * because a node entry needs nothing else from them.
 *
 * WHERE A SEGMENT STOPS BEING AUTHORITATIVE
 * ------------------------------------------
 * An annotation names a subtree, so its precision is one node entry -- 2 MiB
 * at level 1 on Sv39.  A VMA split (munmap in the middle of a mapping) puts a
 * boundary at an arbitrary page address, and the straddling entry can only be
 * labelled with one side's segment; the other side's pages sit inside an entry
 * that claims the wrong file offset.
 *
 * Rather than pretend otherwise, the segment carries the extent it actually
 * describes and mm_pt_lookup_seg() refuses to answer for an address outside
 * it.  A caller that gets NULL has not been lied to: it falls back to the VMA,
 * exactly as it did before the segment existed.  That is what makes this safe
 * to land before the fault path stops consulting the VMA -- the fallback is
 * still there and is still correct, it just becomes rarer.
 *
 * A leaf entry never carries a segment: once a page exists, its class already
 * says what it is, and the segment is only needed to resolve pages that have
 * not been backed yet.
 */
struct vmo;
struct vnode;


/* Segments nameable by ONE node entry.  This is the width of the packed index,
 * not the capacity of the table -- the two were the same number until the
 * segment array was split out, and conflating them is what made this a
 * capacity problem when it was a sharing one (see below).
 *
 * Seven is the frame's ceiling, not a chosen number.  idx[] is 512 entries
 * living in one order-0 frame alongside `arr`: seven names each is 3592 bytes,
 * eight is 4104 and overruns.  The segtab's own refcount was dropped to buy
 * back the eight bytes that cost, and even then eight does not fit.
 *
 * Measured, one step at a time -- each step moves the ceiling rather than
 * removing it, which is why the limits are measured rather than assumed:
 *
 *     4 names/entry, 8 shared slots   seg_miss 1992   nibbles_full    0
 *     4 names/entry, 255 shared slots seg_miss  ~900  nibbles_full 5900
 *     7 names/entry, 255 shared slots seg_miss  ~270  nibbles_full 2900
 *
 * What is left is not a bug to be tuned away.  A node entry is 2 MiB on Sv39,
 * so naming a mapping means naming it at 2 MiB resolution: an entry holding
 * more than seven distinct mappings cannot say which is which, and the lookup
 * correctly declines rather than guessing.  Closing that needs finer
 * resolution -- annotating the leaf, or splitting the index out of the frame --
 * not a larger constant. */
#define MM_SEGTAB_NAMES 7

/* Distinct segments nameable by one page-table page's shared array.  A node
 * entry is 2 MiB at level 1 on Sv39, so one node page spans a gigabyte and its
 * array has to cover every mapping in it.  Measured on the real-software gate:
 * with only eight, segtab_slot() reported the table full 7132 times and every
 * one of those was an address that then had to fall back to the VMA -- while
 * per-entry names were never exhausted at all (0 nibble-full events).  The
 * binding constraint was the shared array, never the per-entry index.
 *
 * This lives in its own refcounted allocation rather than inline, so the
 * per-node-page segtab is just the index array and both sizes can move
 * independently. */
#define MM_SEGTAB_MAX 255

/* Per-page-table-page segment table, allocated on first annotation.
 *
 * NO refcount, unlike the mm_segarr beside it.  A segtab is owned by exactly
 * one pt_meta_t, which frees it when that PT page goes away, so a count would
 * be written once and never read.  Dropping it is what buys the eight bytes
 * a wider index needs: with the field still here the struct is 4104 bytes even
 * at MM_SEGTAB_NAMES=8, which overruns the order-0 frame it is allocated from
 * by exactly eight. */
typedef struct mm_segtab {
    /* The segments named by this node page's entries, shared by all of them.
     * Refcounted: several node pages can be annotated from one mmap, so the
     * array outlives any single segtab that points at it. */
    struct mm_segarr *arr;
    /* Index is 1-based so that 0 means "no segment", which is also the state
     * of every entry in a table that never needed one.
     *
     * SIX bytes per entry: MM_SEGTAB_NAMES eight-bit slot numbers.  It was
     * ONE byte holding four-bit slots, and the packer cast its result back to
     * that width, so slots three and four were written and read back as
     * nothing.  Nothing could detect it -- the annotate walk, the lookup and
     * the auditor all read back the same truncated byte -- and it showed up as
     * a seg_miss that no amount of walking would close.  segtab_packed_t in
     * pt.c exists so the next width mistake is a compile error.
     *
     * The segtab is allocated from a SINGLE order-0 frame and this array is
     * 512 entries of it, so the static assert in pt.c is what stops the whole
     * structure from quietly overrunning that frame -- which surfaces as a wild
     * pointer in proc_put, not as anything to do with segments. */
    /* Six bytes, not eight: see MM_SEGTAB_NAMES.  A byte array rather than a
     * packed integer, so the slot accessors index it directly and a width
     * mistake is a compile error instead of a silent truncation. */
    uint8_t      idx[MM_PT_META_ENTRIES * MM_SEGTAB_NAMES];
} mm_segtab_t;

_Static_assert(MM_SEGTAB_NAMES <= 7,
               "idx[] fits seven names in the frame; widen the layout before adding one");
_Static_assert(MM_SEGTAB_MAX <= 255, "a slot number must fit the byte idx[] gives it");

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
    uint64_t vmai_mismatch;  /* status claims a page no VMA accounts for */
    uint64_t safe_mismatch;  /* MM_SAFE_NO_FA disagrees with VM_SEALED */
    /* Present leaf whose status class the covering VMA could not have
     * produced (anonymous inside a MAP_SHARED file VMA, or vice versa).
     * This is the precondition for P6: fault dispatch may only start
     * deciding "anonymous or file?" from the status while this is 0. */
    uint64_t cls_mismatch;
    /* Not an error: how many leaves carry MM_AS_ANON_VIRT, i.e. are reserved
     * but not yet backed.  This is the on-demand paging state the paper
     * relies on, so it is counted to make it observable rather than inferred
     * from the absence of mismatches. */
    uint64_t anon_virt;
    /* Segment annotations (P6).  seg_slots counts PT-node entries that name a
     * segment; seg_pages counts distinct segments those entries resolve to.
     * Both are observations, not verdicts -- a purely anonymous address space
     * has zero of each and is fine.
     *
     * seg_bad_slot IS a verdict: a non-zero index that does not resolve to a
     * live mm_seg_t means fault dispatch would dereference a freed vnode the
     * first time it trusted the annotation.  seg_kind_mismatch is the same
     * idea one level up: the segment says FILE where the covering VMA says
     * VMO, so the reader would take the wrong branch for the page. */
    uint64_t seg_slots;
    uint64_t seg_pages;
    uint64_t seg_bad_slot;
    uint64_t seg_kind_mismatch;
    /* Is the mapping list well formed?
     *
     * The list is the only representation of the address space now, and
     * mm_seg_find() binary-searches it, so a list that is unsorted, has
     * overlapping entries, or holds a dead record answers with the wrong
     * mapping and nothing else is left to catch it.  vmas counts the entries
     * walked; noseg counts records whose magic is gone or whose extent is
     * inverted; extent_mismatch counts entries that overlap their predecessor.
     * agree/disagree count entries that passed. */
    uint64_t seg_extent_vmas;
    uint64_t seg_extent_mismatch;
    uint64_t seg_extent_noseg;
    uint64_t seg_pte_agree;      /* entries that passed the check above */
    uint64_t seg_pte_disagree;   /* unused; kept so the report layout is stable */
    vaddr_t seg_bad_va;
    vaddr_t seg_kind_bad_va;
    uint8_t  seg_kind_bad;
    /* First offending address per counter.  A bare count says "some VMA is
     * inconsistent"; an address says which mutator to read.  The audit runs
     * only on the shutdown path and on the gate's explicit request, so the
     * extra words cost nothing that matters. */
    vaddr_t vma_bad_va;
    vaddr_t vmai_bad_va;
    vaddr_t cls_bad_va;
    uint8_t cls_bad_class;
} mm_pt_audit_report_t;

static inline uint64_t mm_pt_audit_errors(const mm_pt_audit_report_t *r)
{
    return r->missing_meta + r->present_mismatch + r->absent_mismatch +
           r->prot_mismatch + r->cow_mismatch + r->vma_mismatch +
           r->vmai_mismatch + r->cls_mismatch +
           r->safe_mismatch + r->seg_bad_slot + r->seg_kind_mismatch +
           r->seg_extent_mismatch + r->seg_extent_noseg;
}


#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)

/* Descriptor access.  table is a direct-mapped pointer to a page-table page. */
pt_meta_t *mm_pt_meta(pte_t *table);
static inline int mm_pt_meta_level(const pt_meta_t *m) { return m->level; }
static inline int mm_pt_meta_stale(const pt_meta_t *m)  { return m->stale; }
void mm_pt_meta_set_stale(pt_meta_t *m, int stale);
void mm_pt_node_lock(pte_t *table);
void mm_pt_node_unlock(pte_t *table);

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

/* ---- backing-object segments (P6) ----
 *
 * Annotate the node entries covering [start, end) with `seg`, and resolve the
 * segment backing an address.  Both are described in mm/pt.h above; the short
 * version is that the index lives on PT-NODE entries so that recording the
 * kind of a mapping costs one write per node rather than one per page.
 *
 * mm_pt_annotate_seg() touches no PTE and takes its own per-node locks, so it
 * is not a cursor operation; it is called with mm->lock held.  It descends
 * only into nodes that already exist, so run it AFTER the mapping's path is
 * installed.
 *
 * mm_pt_lookup_seg() returns a referenced segment (NULL for anonymous), which
 * is what lets a reader resolve the object after dropping mm->lock; pair it
 * with mm_seg_put(). */
int mm_pt_annotate_seg(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                       mm_seg_t *seg);
mm_seg_t *mm_pt_lookup_seg(mm_struct_t *mm, vaddr_t addr);
/* Drop annotations covering [start, end).  munmap needs this because the node
 * collapse only runs for tables that hold no leaves, and the mappings a
 * segment exists for are precisely the ones that hold none.
 *
 * `only` names the segment giving up its claim; NULL drops every name on the
 * entries in the range.  Pass the segment wherever the caller knows it, because
 * an entry is coarser than a mapping and may also be naming a neighbour that is
 * still perfectly well described. */
void mm_pt_unannotate_seg(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                          mm_seg_t *only);
/* Drop one entry's index; for the unmap path that turns a node entry back into
 * a leaf.  Caller holds the parent node's lock. */
void mm_pt_node_clear_seg(pte_t *table, int level, int idx);

/* Mapping-record lifetime.  mm_seg_new() (mm/vma.c) creates one with a single
 * reference the creator owns; the index and every annotation walk take their own
 * with mm_seg_get(), and the last mm_seg_put() runs the record's release
 * callback and frees it. */
mm_seg_t *mm_seg_get(mm_seg_t *s);
void      mm_seg_put(mm_seg_t *s);

/* Shadow check (P6, docs 12.6).  Asking the segment what it would have said,
 * and comparing against what the VMA actually said, turns "the segment is
 * good enough to replace the VMA" from a claim into a number.  It is a
 * measurement, not a code path: nothing here changes what the fault does.
 *
 * `kind` is MM_SEG_FILE or MM_SEG_VMO (what the caller believes the mapping
 * is), `off` the object offset it computed for `addr`, `shared` the MAP_SHARED
 * flag.  Returns 1 if a segment covered the address AND agreed on all three,
 * 0 if it covered the address and disagreed, -1 if none covered it (the
 * straddling-entry case in "WHERE A SEGMENT STOPS BEING AUTHORITATIVE").
 *
 * On a disagreement, and only for the first MM_SEG_SHADOW_REPORT of them,
 * `*found` receives the offending segment with a reference the CALLER must
 * drop.  The caller is the only side that can name a VMA, so it is the only
 * side that can print a comparison worth reading. */
#define MM_SEG_SHADOW_REPORT 8
int mm_pt_shadow_seg(mm_struct_t *mm, vaddr_t addr, uint8_t kind,
                     uint64_t off, int shared, mm_seg_t **found);

/* Counters behind those verdicts, printed in [MM-ASM].  shadow_miss counts
 * faults that asked and got nothing, which is the number that says how much of
 * the workload the segment table still cannot serve. */
extern uint64_t mm_seg_shadow_agree;
extern uint64_t mm_seg_shadow_disagree;
extern uint64_t mm_seg_shadow_miss;

/* Which side of the P6 dispatch actually decided.  Both zero means the change
 * is inert; both equal means the segment is inert. */
extern uint64_t mm_seg_dispatch_seg;
extern uint64_t mm_seg_dispatch_fallback;

/* Why a lookup found nothing, and why an annotation could not be recorded.
 *
 * These earned their place by refuting a diagnosis: the residual seg_miss was
 * attributed to entries running out of names, and mm_seg_annot_lost[1] (per-entry
 * names exhausted) measured ZERO while mm_seg_annot_lost[0] (the shared array
 * exhausted) measured 7132.  Every capacity fix since has been chosen against
 * these numbers rather than against the frame arithmetic, which pointed at the
 * wrong limit the whole way.  Both are printed in [MM-ASM].
 *
 *   mm_seg_miss_why[]  0 hole  1 leaf  2 unnamed  3 extent  4 ambiguous
 *                      5 fell off the bottom
 *   mm_seg_annot_lost[] 0 shared array full  1 entry's names all taken
 *   mm_seg_full_lvl[]  per level, how many annotations that level could not take
 */
enum {
    MM_MW_HOLE = 0,      /* entry not valid */
    MM_MW_LEAF,          /* a huge leaf, which never names a segment */
    MM_MW_UNNAMED,       /* node has an array, this entry names nothing */
    MM_MW_EXTENT,        /* named, but no name covers the address */
    MM_MW_AMBIG,         /* two live names both cover it */
    MM_MW_BOTTOM,        /* walked off the bottom, still nothing */
    MM_MW_COUNT
};
extern uint64_t mm_seg_miss_why[MM_MW_COUNT];
extern uint64_t mm_seg_annot_lost[2];
extern uint64_t mm_seg_full_lvl[8];

/* Per-PTE metadata maintenance.  mm_pt_note_present() and
 * mm_pt_note_absent() bracket every PTE write; the level-0 helpers are the
 * leaf forms.  The AUDIT-only entry point exists so the auditor can compare
 * the two representations without going through a cursor. */
void mm_pt_note_present(pte_t *table, int level, int idx, uint8_t cls_byte);
void mm_pt_note_absent(pte_t *table, int level, int idx);
/* Re-derive protection and the COW bit from a PTE a non-cursor writer just
 * rewrote, keeping the class the caller already knows.  Every direct-PTE
 * writer outside pt.c (mprotect, cow, madvise, demote) must call this after
 * its store, or the status silently keeps describing the old page. */
int mm_pt_sync_status(pte_t *table, int level, int idx, uint8_t cls);
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
int mm_pt_refresh_leaf_prot(pte_t *table, int idx, pte_t ptef);

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
