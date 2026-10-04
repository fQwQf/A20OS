#include "core/defs.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/cpu.h"
#include "core/perf.h"
#include "core/string.h"
#include "core/bootargs.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/mm.h"
#include "mm/pt.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#ifdef CONFIG_SWAP
#include "mm/swap.h"
#endif

/*
 * Single-level address space core (MM_AS_MODEL, see mm/pt.h).
 *
 * This file owns:
 *   - the page-table page descriptor (pt_meta_t) and its lifecycle,
 *   - the per-PTE metadata array, i.e. the authoritative per-virtual-page
 *     status that replaces the VMA as the source of truth for mapping state,
 *   - the transactional cursor, the only supported way to program the MMU,
 *   - the invariant auditor that proves the two representations agree.
 *
 * Paging differences are hidden behind the arch helpers in
 * arch/ARCH/include/page_table.h, so this file is architecture generic.
 */

/* Eager-provisioning cap (pages; 0 = off).  Plain state with no
 * page-table dependency, so it lives outside the pgtable guard: the
 * /proc/a20/anonprov knob must exist on NOMMU too (where writing
 * it returns -ENOSYS but reading it still reports the cap). */
#ifdef CONFIG_ANON_PROV_DEFAULT
static uint32_t g_anon_prov_max = CONFIG_ANON_PROV_DEFAULT;
#else
static uint32_t g_anon_prov_max;      /* off by default */
#endif

/* Runtime view of the eager-provisioning cap.  Read on every mmap, so it is
 * published with acquire semantics against mm_pt_set_anon_prov_max()'s
 * release store -- that ordering is what lets /proc/a20/anonprov flip the cap
 * between benchmark iterations inside one boot (docs 10.70/10.71). */
uint32_t mm_pt_anon_prov_max(void)
{
    return __atomic_load_n(&g_anon_prov_max, __ATOMIC_ACQUIRE);
}

/* Set the cap at runtime.  Only meaningful on a build that has the per-PTE
 * status sidecar: under NOMMU there is no page table, and on an architecture
 * whose page-table backend predates the sidecar (arm32's short-descriptor
 * walker) there is no status byte to mark MM_ST_ANON_VIRT in and no
 * mm_pt_provision_anon() to read the cap at all.  In both cases the capability
 * does not exist, so the write is refused with -ENOSYS instead of being
 * accepted and silently ignored -- a knob that accepts a value nothing reads
 * is a fabricated capability (docs/security/hardening.md).  The read side stays
 * available everywhere and reports 0, which is the truth: provisioning off. */
int mm_pt_set_anon_prov_max(uint32_t pages)
{
#if defined(CONFIG_NOMMU) || !defined(ARCH_HAS_PGTABLE_OPS)
    (void)pages;
    return -ENOSYS;
#else
    __atomic_store_n(&g_anon_prov_max, pages, __ATOMIC_RELEASE);
    return 0;
#endif
}

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)

/* ------------------------------------------------------------------ *
 * MCS queue lock
 * ------------------------------------------------------------------ *
 * A cursor's preorder DFS can hold a whole page-table subtree at once -- up
 * to one node per covered sub-range, so hundreds for a large transaction.
 * A fixed pool sized by ARCH_PT_LEVELS would overflow, and nesting needs one
 * node per simultaneously held lock.  Nodes are therefore per (cpu, depth)
 * with a per-CPU array that grows on demand; only the acquiring CPU ever
 * touches a node, so no cross-CPU synchronisation is involved.
 */
typedef struct pt_mcs_node {
    volatile uintptr_t next;
    volatile uintptr_t locked;
} pt_mcs_node_t;

/* Worst-case simultaneous page-table locks held by one CPU: a preorder DFS
 * over a covering node's subtree.  The covering node contributes one per
 * level down to the leaf, and the widest fan-out is one level-1 node with all
 * 512 of its level-0 children present.  Sized for that plus headroom, and
 * fixed at init: the lock path runs with preemption disabled, so it must
 * never call an allocator. */
#define PT_MCS_POOL_SLOTS 1024

typedef struct pt_mcs_pool {
    pt_mcs_node_t *nodes;
    pt_meta_t    **held;      /* parallel stack: node held at each depth */
    uint32_t       depth;
} pt_mcs_pool_t;

static pt_mcs_pool_t g_pt_mcs_pool[CONFIG_NR_CPUS];

/* Pre-size every CPU's node pool at init so the lock path never allocates.
 * A deep DFS on a large transaction can hold hundreds of nodes; sizing for
 * the worst realistic case up front keeps mcs_lock allocation-free, which
 * matters because it runs with preemption disabled and page-table locks held. */
static void pt_mcs_pool_init(void)
{
    for (unsigned c = 0; c < CONFIG_NR_CPUS; c++) {
        pt_mcs_node_t *n = kmalloc(PT_MCS_POOL_SLOTS * sizeof(pt_mcs_node_t));
        pt_meta_t **h = kmalloc(PT_MCS_POOL_SLOTS * sizeof(pt_meta_t *));
        if (!n || !h)
            panic("mcs: cannot allocate initial per-cpu node pool");
        g_pt_mcs_pool[c].nodes = n;
        g_pt_mcs_pool[c].held = h;
        g_pt_mcs_pool[c].depth = 0;
    }
}

static inline unsigned pt_cpu(void)
{
    unsigned cpu = cpu_current_id();
    return cpu < CONFIG_NR_CPUS ? cpu : 0;
}

/* The MCS node is stashed in the descriptor while the lock is held so that
 * unlock can find it; only the holder ever touches that field. */
static void mcs_lock(pt_meta_t *m)
{
    unsigned cpu = pt_cpu();
    pt_mcs_pool_t *pool = &g_pt_mcs_pool[cpu];
    uint32_t d = pool->depth;

    /* The pool is sized at init for the worst-case DFS width, so this is a
     * "cannot happen" guard.  It must not grow here: the lock path runs with
     * preemption disabled and must never call an allocator. */
    if (d >= PT_MCS_POOL_SLOTS)
        panic("mcs: page-table lock nesting exceeded the per-cpu pool");

    pt_mcs_node_t *me = &pool->nodes[d];
    pool->held[d] = m;
    /* One push per acquisition.  The matching pop belongs to the cursor's
     * unwind loop in mm_cursor_unlock, not to mcs_unlock, so that the two
     * steps -- hand the node to the next waiter, and forget the slot -- stay
     * distinct and the depth cannot be decremented twice. */
    pool->depth = d + 1;

    me->next = 0;
    me->locked = 1;

    uintptr_t tail = __atomic_exchange_n(&m->lock, (uintptr_t)me,
                                         __ATOMIC_ACQ_REL);
    a20_perf_count(A20_PERF_MM_PT_LOCK_ACQUIRES);
    if (tail) {
        a20_perf_count(A20_PERF_MM_PT_LOCK_CONTENDED);
        a20_perf_count(A20_PERF_MM_PT_LOCK_WAITS);
        __atomic_store_n(&me->locked, 0, __ATOMIC_RELEASE);
        /* Link behind the predecessor, or the queue never exists.  Without this
         * the unlocker finds me->next == 0 and takes the compare-exchange
         * branch -- but m->lock was already moved to THIS waiter by the
         * exchange above, so that CAS always fails, the lock is never released,
         * and this waiter is never handed the lock.  One contended acquisition
         * wedges the node permanently. */
        __atomic_store_n(&((pt_mcs_node_t *)tail)->next, (uintptr_t)me,
                         __ATOMIC_RELEASE);
        uint32_t spins = 0;
        while (__atomic_load_n(&me->locked, __ATOMIC_ACQUIRE) == 0) {
            arch_cpu_relax();
            /* Bounded spin.  A page-table lock is never held across a
             * blocking operation, so a long wait means a lock-order bug --
             * except under KVM, where the holder can be descheduled by the
             * host and make a legitimate wait look arbitrarily long.  Report
             * whether the node we wait on is one this CPU already holds: that
             * is the self-deadlock signature, and it is the only condition
             * that justifies aborting. */
            if (++spins == (1u << 26)) {
                int mine = (d > 0 && pool->held[d - 1] == m);
                if (mine) {
                    kerr("[MCS DEADLOCK] cpu=%u SELF node=%p level=%u "
                         "depth=%u\\n", cpu, (void *)m, m->level,
                         (unsigned)d);
                    panic("page-table lock self-deadlock");
                }
                /* Remote holder: not a deadlock, keep waiting.  Reset the
                 * counter so the watchdog only fires on a true self-lock. */
                spins = 0;
            }
        }
    }
}

static void mcs_unlock(pt_meta_t *m)
{
    pt_mcs_pool_t *pool = &g_pt_mcs_pool[pt_cpu()];
    /* Our own node, NOT m->node.  Every acquirer overwrote that shared field
     * with its own pointer, so a holder reading it back got the newest waiter
     * instead of itself -- it then cleared the lock and handed off to nobody,
     * and that waiter span forever.  mcs_lock pushed exactly one slot and
     * depth is decremented only below, so our node is the top of our stack. */
    pt_mcs_node_t *me = &pool->nodes[pool->depth - 1];

    pt_mcs_node_t *next =
        (pt_mcs_node_t *)__atomic_load_n(&me->next, __ATOMIC_ACQUIRE);
    if (next) {
        __atomic_store_n(&next->locked, 1, __ATOMIC_RELEASE);
    } else {
        /* We are the tail.  A compare-exchange rather than a store, so a
         * concurrent enqueue that already read the tail cannot be lost. */
        uintptr_t self = (uintptr_t)me;
        if (!__atomic_compare_exchange_n(&m->lock, &self, 0, 0,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            /* Lost the race: a successor swapped itself in after we read
             * me->next but before we cleared m->lock.  It is now blocked writing
             * our ->next, so wait for that link to appear and hand off.  Without
             * this wait the successor waits forever on a lock nobody releases. */
            while (__atomic_load_n(&me->next, __ATOMIC_ACQUIRE) == 0)
                arch_cpu_relax();
            next = (pt_mcs_node_t *)__atomic_load_n(&me->next, __ATOMIC_ACQUIRE);
            __atomic_store_n(&next->locked, 1, __ATOMIC_RELEASE);
        }
    }
    /* Every acquisition pushed exactly one slot in mcs_lock, so every release
     * pops exactly one here.  This is the ONLY place the depth is
     * decremented: the descent loop's inline lock/unlock pairs and the
     * cursor's unwind loop both rely on it, and pre-decrementing in a caller
     * as well is a double decrement that corrupts the stack discipline. */
    pool->depth--;
}

/* ------------------------------------------------------------------ *
 * Descriptor lifecycle
 * ------------------------------------------------------------------ */
static void segtab_detach_locked(pt_meta_t *m);
static void segtab_maybe_free_locked(pt_meta_t *m);

/* The array of segments a node page names, shared by all 512 of its entries.
 *
 * This was inline in mm_segtab_t, which capped the whole address space's
 * expressible segments at one node page's worth -- and a level-1 node page
 * covers a gigabyte, so eight slots were never going to cover a compiler's
 * mappings.  Measured: segtab_slot() reported "full" 7132 times on the
 * real-software gate, every one of which became a fallback to the VMA.
 *
 * Splitting it out makes the two limits independent: the per-entry index still
 * bounds how many segments ONE entry may name (MM_SEGTAB_NAMES, because the
 * names have to be packed into idx[]), while the array bounds how many distinct
 * segments the whole node page may refer to.  Only the second was binding. */
struct mm_segarr {
    volatile int refcount;
    uint8_t      n;                    /* slots in use, 0..MM_SEGTAB_MAX */
    mm_seg_t    *seg[MM_SEGTAB_MAX];
};

static struct mm_segarr *segarr_alloc(void)
{
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE)
        return NULL;
    struct mm_segarr *a = (struct mm_segarr *)pfn_to_virt(pfn);
    memset(a, 0, sizeof(*a));
    a->refcount = 1;
    return a;
}

/* Drop one reference, releasing the segments and the page with the last.  Every
 * segment in the array is put exactly once here, which is why `n` counts
 * distinct segments rather than annotations: a page naming one segment on 400
 * entries holds one reference to it, not 400. */
static void segarr_put(struct mm_segarr *a)
{
    if (!a)
        return;
    if (--a->refcount > 0)
        return;
    for (uint8_t i = 0; i < a->n; i++)
        mm_seg_put(a->seg[i]);
    pfa_free(virt_to_pfn(a), 0);
}

void mm_pt_core_init(void)
{
    pt_mcs_pool_init();
}

pt_meta_t *mm_pt_meta(pte_t *table)
{
    if (!table)
        return NULL;
    pfn_t pfn = virt_to_pfn((const void *)table);
    if (!pfn_valid(pfn) || pfa.meta[pfn].flags != FRAME_F_PT)
        return NULL;
    return pfa.meta[pfn].pt;
}

void mm_pt_meta_set_stale(pt_meta_t *m, int stale)
{
    if (m)
        __atomic_store_n(&m->stale, stale ? 1u : 0u, __ATOMIC_RELEASE);
}

/* Take/release the lock of the PT node reached by `table`, for callers that
 * descend to their own node instead of going through a cursor -- mcs_lock() is
 * file-local, so pt_unmap_leaf()/pt_unmap() in mm.c cannot reach it directly.
 *
 * Same node, and same reason, as cursor_leaf_slot(): a write touches one leaf
 * entry plus that node's own nr_present/cls[] read-modify-write.  A node with no
 * metadata has no lock, same as there.
 *
 * One at a time: cursor_leaf_slot() takes these in descending level order, so
 * nesting one here would invert against it. */
void mm_pt_node_lock(pte_t *table)
{
    pt_meta_t *pm = mm_pt_meta(table);
    if (pm)
        mcs_lock(pm);
}

void mm_pt_node_unlock(pte_t *table)
{
    pt_meta_t *pm = mm_pt_meta(table);
    if (pm)
        mcs_unlock(pm);
}

static int pt_meta_order(size_t bytes)
{
    int order = 0;
    size_t span = PAGE_SIZE;
    while (span < bytes) {
        if (order >= MAX_ORDER)
            return -1;
        span <<= 1;
        order++;
    }
    return order;
}

int mm_pt_node_init(pte_t *table, int level)
{
    if (!table)
        return -EINVAL;
    pfn_t pfn = virt_to_pfn((const void *)table);
    if (!pfn_valid(pfn))
        return -EINVAL;

    int order = pt_meta_order(sizeof(pt_meta_t));
    pfn_t mpfn = pfa_alloc(order);
    if (mpfn == PFN_NONE)
        return -ENOMEM;

    pt_meta_t *m = (pt_meta_t *)pfn_to_virt(mpfn);
    memset(m, 0, sizeof(*m));
    m->level = (uint8_t)level;

    /* Publish under pfa.lock so a concurrent descriptor lookup can never see
     * FRAME_F_PT with a NULL pointer. */
    uint64_t flags = spin_lock_irqsave(&pfa.lock);
    pfa.meta[pfn].pt = m;
    pfa.meta[pfn].flags = FRAME_F_PT;
    spin_unlock_irqrestore(&pfa.lock, flags);
    return 0;
}

void mm_pt_node_fini(pte_t *table)
{
    if (!table)
        return;
    pfn_t pfn = virt_to_pfn((const void *)table);
    if (!pfn_valid(pfn))
        return;

    uint64_t flags = spin_lock_irqsave(&pfa.lock);
    pt_meta_t *m = pfa.meta[pfn].pt;
    pfa.meta[pfn].pt = NULL;
    pfa.meta[pfn].flags = FRAME_F_ALLOC;
    spin_unlock_irqrestore(&pfa.lock, flags);

    if (m) {
        /* Drop the backing-object references this page-table page held.
         * The page is already detached (FRAME_F_PT cleared, frame marked
         * stale by the unlink before this runs), so no cursor can reach the
         * segtab and the node lock is not needed to tear it down.  Forgetting
         * this leaks one segment reference per PT page a file mapping spans --
         * and, worse, keeps a vnode alive that munmap was supposed to drop. */
        segtab_detach_locked(m);
        pfa_free(virt_to_pfn(m), pt_meta_order(sizeof(pt_meta_t)));
    }
}

/* ------------------------------------------------------------------ *
 * Per-PTE metadata
 * ------------------------------------------------------------------ */
static inline uint8_t *cls_slot(pt_meta_t *m, int idx)
{
    if (!m || idx < 0 || idx >= MM_PT_META_ENTRIES)
        return NULL;
    return &m->cls[idx];
}

static inline uint8_t *safe_bit(pt_meta_t *m, int idx)
{
    if (!m || idx < 0 || idx >= MM_PT_META_ENTRIES)
        return NULL;
    return &m->safe[idx >> 3];
}

/* ------------------------------------------------------------------ *
 * Backing-object segments (P6)
 * ------------------------------------------------------------------ *
 * Type layout and the reason the index lives on node entries rather than
 * leaves are in mm/pt.h.  This is the storage and the refcount; the walk that
 * annotates a range is further down, next to the cursor it has to share
 * locking rules with.
 */

/* Release the record itself.  Reached only from mm_seg_put(), after the
 * `release` callback has dropped the backing vnode/vmo. */
static void seg_free(mm_seg_t *s)
{
    if (!s || s->magic != MM_SEG_MAGIC)
        return;
    s->magic = 0;

    kfree(s);
}

mm_seg_t *mm_seg_get(mm_seg_t *s)
{
    if (s)
        refcount_inc(&s->refcount);
    return s;
}

void mm_seg_put(mm_seg_t *s)
{
    if (!s)
        return;
    /* A put on a record whose magic is gone is a use-after-free: the slab
     * object was recycled, so `refcount` is somebody else's number and the
     * branch below is deciding whether to call a stale `release` pointer.  That is exactly the
     * shape of a crash the real-software gate took while anonymous mappings
     * carried segments -- a wild jump to 0x2f0a7d203b303220, which is ASCII
     * file text, not code, because `release` had been read off a page-cache
     * page.  Dying here names the fault instead of leaving it to a frame-pointer
     * walk through the middle of it. */
    if (s->magic != MM_SEG_MAGIC)
        panic("mm_seg_put: use-after-free, mapping record %p magic=0x%x",
              s, s->magic);
    if (refcount_dec_and_test(&s->refcount)) {
        /* The record owns one reference on whatever backs it (a vnode, a VMO),
         * taken by whoever created the mapping.  This file has no business
         * knowing about those types, so the release is a callback -- and it is
         * installed by mm_seg_new() in mm/vma.c rather than by the creator,
         * because a record that reaches zero without one would leak the
         * backing object.
         *
         * It runs here only when nothing else owes it.  A record whose list
         * reference has already been dropped is on the deferred queue, and
         * that queue -- which holds a reference of its own, so this put
         * cannot be the one that frees it -- is what runs the release, in a
         * context that is allowed to block. */
        if (!s->released && s->release) {
            s->released = 1;
            s->release(s);
        }
        seg_free(s);
    }
}

/* Attach the segment table to a page-table page, allocating it on first use.
 * Caller holds that page's node lock (cursor_leaf_slot()'s lock, or the
 * explicit mm_pt_node_lock() taken by the annotating walk). */
static mm_segtab_t *segtab_attach_locked(pt_meta_t *m)
{
    if (m->segtab)
        return m->segtab;
    /* One block holding the segtab and its index together, so the index needs
     * no second allocation and no second failure path: a segtab with no index
     * is not a segtab.  kcalloc rather than a frame, which is what lets the
     * index width be a chosen number -- see MM_SEGTAB_NAMES. */
    mm_segtab_t *st = kcalloc(1, sizeof(*st) +
                              (size_t)MM_PT_META_ENTRIES * MM_SEGTAB_NAMES);
    if (!st)
        return NULL;
    st->idx = (uint8_t *)(st + 1);
    st->arr = segarr_alloc();
    if (!st->arr) {
        kfree(st);
        return NULL;
    }
    m->segtab = st;
    return st;
}

/* Release a page-table page's segment table.  The segments themselves belong to
 * the shared array, which other node pages may still be naming, so this drops
 * the array's reference and lets the refcount decide.
 *
 * Caller must hold the page's node lock, or have established that no cursor
 * can reach the page (mm_pt_node_fini, which runs on the detached frame). */
static void segtab_detach_locked(pt_meta_t *m)
{
    mm_segtab_t *st = m->segtab;
    if (!st)
        return;
    m->segtab = NULL;
    segarr_put(st->arr);
    /* idx is not a separate allocation -- it is the tail of this block, so
     * freeing the segtab frees both and there is no second pointer to get
     * wrong. */
    kfree(st);
}

/* Find or add `s` in the node page's shared array, returning its 1-based slot,
 * or 0 if that array is full.  A hard cap: silently overwriting a slot would
 * make one mapping's pages inherit another mapping's vnode. */
static uint8_t segtab_slot(mm_segtab_t *st, mm_seg_t *s)
{
    struct mm_segarr *a = st->arr;
    if (!a)
        return 0;
    for (uint8_t i = 0; i < a->n; i++)
        if (a->seg[i] == s)
            return (uint8_t)(i + 1);
    if (a->n >= MM_SEGTAB_MAX)
        return 0;
    a->seg[a->n] = mm_seg_get(s);
    return (uint8_t)(++a->n);
}

/* ---- Entry index packing ------------------------------------------------
 *
 * mm_segtab_t.idx[] is a flat byte array: slot k of entry i is
 * idx[i * MM_SEGTAB_NAMES + k].  A slot number is 1..MM_SEGTAB_MAX, so 0 means
 * "this slot is empty" and costs no separate bitmap.
 *
 * The slot width and the number of slots are now different limits, which is
 * the whole point of the split.  A slot number addresses the node page's shared
 * array (up to MM_SEGTAB_MAX of them), while the per-entry index has to fit
 * every name that entry carries in a fixed four bytes.
 *
 * It was ONE byte of FOUR-BIT slots, which is two nibbles -- and the packer
 * cast its result back to the parameter type, so segments three and four could
 * be written and then read back as nothing.  The truncation was silent because
 * the auditor reads back the same byte the writer wrote, so no layer could see
 * it.  The visible symptom was a seg_miss that stayed put no matter how the
 * annotate walk was changed.  A flat byte array has no width to get wrong.
 *
 * Why one entry needs more than one name: a node entry is far coarser than a
 * mapping.  A level-1 entry covers 2 MiB on Sv39, and two separate mmap calls
 * routinely land side by side inside one -- say a loader mapping a library at
 * offset 0 and the next object at offset 0x9000.  With a single name the entry
 * can only say one of them, and a fault in the other resolves to a segment
 * describing the wrong file offset.  That was not hypothetical: it is what the
 * P6 shadow check caught, six times over, on the first touch of each such
 * range.  Measured before the change: seg_diff=6 on the real-software gate,
 * every one of them a stale name for a neighbouring mapping.
 *
 * The disambiguation happens in mm_pt_lookup_seg(): it tries each named segment
 * and takes the one whose recorded extent actually contains the address.  Two
 * mappings that share an entry therefore stay separately answerable, and the
 * fallback to the VMA is reserved for the addresses where the answer really is
 * ambiguous. */
/* The shared array is a frame of its own and holds MM_SEGTAB_MAX whole slot
 * pointers, so it has the same bound. */
_Static_assert(sizeof(struct mm_segarr) <= 4096,
               "mm_segarr no longer fits in the single frame it is allocated from");

static inline uint8_t segtab_slot_get(const mm_segtab_t *st, int i, int k)
{
    return st->idx[i * MM_SEGTAB_NAMES + k];
}

static inline void segtab_slot_set(mm_segtab_t *st, int i, int k, uint8_t slot)
{
    st->idx[i * MM_SEGTAB_NAMES + k] = slot;
}

/* Name `slot` on entry `i`, in a free slot.  Idempotent: an entry that
 * already names this segment is left alone, so re-annotating a range costs
 * nothing and does not consume a slot. */
static int segtab_entry_name(mm_segtab_t *st, int i, uint8_t slot)
{
    for (int k = 0; k < MM_SEGTAB_NAMES; k++) {
        uint8_t v = segtab_slot_get(st, i, k);
        if (v == slot)
            return 1;
        if (!v) {
            segtab_slot_set(st, i, k, slot);
            return 1;
        }
    }
    /* Every nibble is taken.  Each already names a live segment, so there is
     * nothing to drop: evicting one would mislabel whichever mapping owns it,
     * and leaving it out only costs coverage on addresses this entry covers
     * anyway.  The caller reports this as a lost annotation. */
    return 0;
}


/* True if entry `i` already names `slot`. */
static int segtab_entry_names(const mm_segtab_t *st, int i, uint8_t slot)
{
    for (int k = 0; k < MM_SEGTAB_NAMES; k++)
        if (segtab_slot_get(st, i, k) == slot)
            return 1;
    return 0;
}

static inline mm_seg_t *segtab_seg(const mm_segtab_t *st, uint8_t slot)
{
    if (!st || !st->arr || slot == 0 || slot > st->arr->n)
        return NULL;
    return st->arr->seg[slot - 1];
}

/*
 * The body of mm_pt_note_present() with the metadata already resolved.  Kept
 * separate so bulk callers (provisioning a whole leaf table) can hoist the
 * mm_pt_meta() lookup out of their per-entry loop instead of paying three
 * dependent loads -- virt_to_pfn, the frame flag check, the .pt deref -- once
 * per page.
 */
static void pt_note_present_meta(pt_meta_t *m, int idx, uint8_t cls_byte)
{
    uint8_t *slot = cls_slot(m, idx);
    if (!slot)
        return;
    if (MM_ST_GET_CLASS(*slot) == MM_ST_INVALID)
        m->nr_present++;
    *slot = cls_byte;
}

void mm_pt_note_present(pte_t *table, int level, int idx, uint8_t cls_byte)
{
    (void)level;
    pt_note_present_meta(mm_pt_meta(table), idx, cls_byte);
}

/*
 * Re-derive the per-PTE status from a PTE that a caller has just rewritten.
 *
 * Several writers legitimately bypass the cursor -- they hold mm->lock, which
 * is still the mutual-exclusion mechanism for everything except the status fast
 * path -- and each of them used to leave the status describing the page as it
 * was BEFORE the rewrite.  A stale byte is not merely untidy: mm_pt_audit_all()
 * reports it as a present/absent/prot/cow mismatch, and once fault dispatch
 * reads the status it installs permissions that disagree with what mprotect or
 * fork asked for.
 *
 * The class comes from the caller because it is not recoverable from the PTE:
 * PTE_R/W/X say what is permitted, not whether the backing is anonymous,
 * file-private or shared, and that distinction is exactly what the status is
 * for.  Protection and the COW bit are derived from the PTE, which is where
 * those facts now live.
 *
 * Returns 0 when the status was refreshed, or a negative errno on a bad class.
 * A table with no metadata (NOMMU, or a page-table page allocated before the
 * model landed) is not an error: there is nothing there to keep in sync.
 */
int mm_pt_sync_status(pte_t *table, int level, int idx, uint8_t cls)
{
    (void)level;
    if (cls >= MM_ST_CLASS_MAX)
        return -EINVAL;
    pt_meta_t *m = mm_pt_meta(table);
    if (!m)
        return 0;
    uint8_t *slot = cls_slot(m, idx);
    if (!slot)
        return 0;

    /* Never resurrect an entry that is not a mapping.  A PT_NODE slot
     * describes a child page-table page rather than a mapping, and INVALID has
     * nothing to describe; writing either would put nr_present and the auditor
     * at odds with the page table. */
    uint8_t cur = MM_ST_GET_CLASS(*slot);
    if (cur == MM_ST_INVALID || cur == MM_ST_PT_NODE)
        return 0;

    pte_t pte = table[idx];
    uint8_t byte = (uint8_t)(MM_ST_CLS_BYTE(cls) |
                             (pte & PTE_COW ? MM_ST_COW_BIT : 0) |
                             mm_pt_prot_bits(pte));
    pt_note_present_meta(m, idx, byte);
    return 0;
}

void mm_pt_note_absent(pte_t *table, int level, int idx)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    uint8_t *slot = cls_slot(m, idx);
    if (!slot)
        return;
    if (MM_ST_GET_CLASS(*slot) != MM_ST_INVALID && m->nr_present)
        m->nr_present--;
    *slot = 0;
    /* Safety bits describe the class that was just cleared, so they must go
     * with it -- otherwise a reused slot would inherit a stale UFFD or
     * NO_FA flag and the fault path would make the wrong decision. */
    uint8_t *sb = safe_bit(m, idx);
    if (sb)
        *sb &= (uint8_t)~MM_SAFE_MASK;
}

int mm_pt_safe_set(pte_t *table, int level, int idx, unsigned flags)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    uint8_t *sb = safe_bit(m, idx);
    if (!sb)
        return -EINVAL;
    /* Refuse to flag a slot that carries no mapping: a safety bit on an
     * INVALID entry has no meaning and would never be cleared. */
    if (MM_ST_GET_CLASS(cls_slot(m, idx) ? *cls_slot(m, idx) : 0) ==
        MM_ST_INVALID)
        return -ENOENT;
    *sb |= (uint8_t)(flags & MM_SAFE_MASK);
    return 0;
}

int mm_pt_safe_clear(pte_t *table, int level, int idx, unsigned flags)
{
    (void)level;
    uint8_t *sb = safe_bit(mm_pt_meta(table), idx);
    if (!sb)
        return -EINVAL;
    *sb &= (uint8_t)~(flags & MM_SAFE_MASK);
    return 0;
}

int mm_pt_safe_test(pte_t *table, int level, int idx, unsigned flags)
{
    (void)level;
    uint8_t *sb = safe_bit(mm_pt_meta(table), idx);
    if (!sb)
        return 0;
    return (*sb & (flags & MM_SAFE_MASK)) == (flags & MM_SAFE_MASK);
}

/*
 * Set or clear per-entry safety bits over a whole virtual range.
 *
 * Walks leaf by leaf (amortising the page-table lookup the way madvise does)
 * rather than looking the leaf up once per page.  `set == 0` clears.
 *
 * Only entries that already carry a mapping are touched: mm_pt_safe_set()
 * refuses an MM_ST_INVALID slot, because a safety bit with no mapping behind
 * it has no meaning and would never be cleared.  That is the right behaviour
 * for both callers -- a page inside a sealed VMA that has not been faulted yet
 * is still legitimately faultable (first touch is not a seal violation), so
 * there is nothing for NO_FA to suppress, and UFFDIO_REGISTER only accepts
 * ranges already backed by a VMA.
 */
int mm_pt_set_safe_range(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                         unsigned flags, int set)
{
    if (!mm || !mm->pgdir || end <= start || !flags)
        return -EINVAL;
    if (start & (PAGE_SIZE - 1) || end & (PAGE_SIZE - 1))
        return -EINVAL;

    for (vaddr_t va = start; va < end; ) {
        int level = 0;
        vaddr_t base = 0;
        size_t size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (!pte || !size) {
            va += PAGE_SIZE;
            continue;
        }
        pte_t *table = pte - arch_pt_vpn(va, 0);
        vaddr_t leaf_end = base + size;
        vaddr_t from = base < start ? start : base;
        vaddr_t to = leaf_end < end ? leaf_end : end;
        for (vaddr_t p = from; p < to; p += PAGE_SIZE) {
            int idx = arch_pt_vpn(p, 0);
            if (set)
                mm_pt_safe_set(table, 0, idx, flags);
            else
                mm_pt_safe_clear(table, 0, idx, flags);
        }
        va = leaf_end < end ? leaf_end : end;
    }
    return 0;
}

/* Clear one page's safety bits.  mm_pt_set_safe_range() clears a whole range
 * in one pass, which is wrong for MM_SAFE_UFFD: a page can still be covered by
 * a different uffd registration, so unregistering one range must not clear a
 * mark another registration still owns.  Callers clear per page and re-test
 * presence between pages (docs 10.59/10.60). */
int mm_pt_safe_clear_page(mm_struct_t *mm, vaddr_t va, unsigned flags)
{
    if (!mm || !mm->pgdir || !flags)
        return -EINVAL;
    if (va & (PAGE_SIZE - 1))
        return -EINVAL;

    int level = 0;
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
    if (!pte || !size)
        return 0;               /* no leaf here: nothing is marked */
    pte_t *table = pte - arch_pt_vpn(va, 0);
    mm_pt_safe_clear(table, 0, arch_pt_vpn(va, 0), flags);
    return 0;
}


uint8_t mm_pt_peek(pte_t *table, int level, int idx)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    uint8_t *slot = cls_slot(m, idx);
    return slot ? *slot : 0;
}

int mm_pt_meta_clone(pte_t *dst_table, pte_t *src_table, int level)
{
    pt_meta_t *src = mm_pt_meta(src_table);
    pt_meta_t *dst = mm_pt_meta(dst_table);
    if (!src || !dst)
        return -EINVAL;
    if (dst->lock != 0)
        return -EBUSY;      /* the clone target must not be visible yet */
    memcpy(dst->cls, src->cls, sizeof(src->cls));
    /* safe[] used to be left out here.  That is a real defect rather than a
     * simplification: MM_SAFE_NO_FA mirrors VM_SEALED, and a forked child that
     * inherited a sealed range would lose the bit -- so the child could fault
     * around inside a range its parent had promised nobody would fault into.
     * The audit's `safe` counter compares the two sides and would report it,
     * which is why this is fixed rather than left as a known gap. */
    memcpy(dst->safe, src->safe, sizeof(src->safe));
    dst->nr_present = src->nr_present;
    dst->level = (uint8_t)level;

    /* Segments are SHARED across the fork boundary, not duplicated: both
     * address spaces describe the same file, so the vnode and the offset
     * sequence are the same objects.  Each side takes its own reference per
     * distinct segment, so the accounting (distinct segments, not annotations)
     * stays right on both.
     *
     * The array itself is COPIED rather than shared, and it has to be: slot
     * numbers are local to the array that holds them, and
     * segtab_entry_forget() renumbers them when it compacts.  Two address
     * spaces sharing one array would mean a munmap in the child renumbering
     * slots the parent's index still refers to -- the child's lookup would
     * silently return a different mapping's segment.  The refcount is
     * therefore about lifetime sharing only, not about sharing the contents. */
    dst->segtab = NULL;
    if (src->segtab) {
        mm_segtab_t *st = segtab_attach_locked(dst);
        if (!st)
            return -ENOMEM;
        struct mm_segarr *sa = src->segtab->arr;
        for (uint8_t i = 0; i < sa->n; i++)
            st->arr->seg[i] = mm_seg_get(sa->seg[i]);
        st->arr->n = sa->n;
        /* An explicit element count, because idx is a pointer now and
         * sizeof(st->idx) is the size of the pointer -- which would have
         * copied sixteen bytes of unrelated struct and left the index
         * uninitialised. */
        memcpy(st->idx, src->segtab->idx,
               (size_t)MM_PT_META_ENTRIES * MM_SEGTAB_NAMES);
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Range classification
 * ------------------------------------------------------------------ *
 * pt_map_kernel() copies the boot PTE words for root entries
 * [ARCH_PT_USER_END, ARCH_PT_ENTRIES) into every address space, so the whole
 * kernel half of a page table is one shared set of physical nodes.  Locking
 * such a node write-locks a structure every process depends on; freeing one
 * is a system-wide use-after-free.  The cursor refuses any range reaching
 * into the shared half, in code rather than in a comment.
 */
int mm_pt_range_is_user(vaddr_t start, vaddr_t end)
{
    vaddr_t limit = (vaddr_t)ARCH_PT_USER_END
                    << (PAGE_SIZE_BITS + ARCH_PT_BITS * ARCH_PT_ROOT_LEVEL);
    if (end <= start)
        return 0;
    return end <= limit;
}

/* ------------------------------------------------------------------ *
 * Page-table descent helpers
 * ------------------------------------------------------------------ */

/* The lowest level whose page-table page completely covers [start,end): the
 * paper's "covering PT page".  Ascending matters -- for a single page that is
 * level 0, and picking the root instead would mean holding the root's lock
 * while mutating unlocked descendants. */
static int pt_covering_level(vaddr_t start, vaddr_t end)
{
    for (int level = 0; level <= ARCH_PT_ROOT_LEVEL; level++) {
        vaddr_t span = (vaddr_t)PAGE_SIZE << (ARCH_PT_BITS * level);
        vaddr_t base = start & ~(span - 1);
        if (base + span >= end)
            return level;
    }
    return ARCH_PT_ROOT_LEVEL;
}

/* Table that owns the leaf slot for addr, reached by a fresh walk from the
 * root.  For code that does not hold a cursor (the legacy pt_map_cls path and
 * the auditor); a cursor uses its cached path instead. */
pte_t *mm_pt_leaf_table(pt_root_t *pgdir, vaddr_t addr)
{
    pte_t *table = pgdir;
    for (int l = ARCH_PT_ROOT_LEVEL; l > 0; l--) {
        pte_t e = table[arch_pt_vpn(addr, l)];
        if (!(e & PTE_V) || arch_pte_is_leaf(e))
            break;
        table = arch_pte_to_ptr(e);
    }
    return table;
}

/* Index the cached path down to the leaf slot for addr, allocating any missing
 * intermediate node, and return with the LEAF TABLE's own lock held.  The
 * caller must pair that with cursor_leaf_unlock() once it has finished reading
 * or writing the slot.
 *
 * Why the leaf is locked here rather than only at mm_addrspace_lock() time: the
 * cursor's covering-node lock does not exclude a peer whose covering node is an
 * ancestor or a descendant of ours.  A wide cursor (covering level 2) and a
 * single-page cursor (covering level 0) inside it would otherwise hold disjoint
 * locks while writing the same leaf PTE and the same pt_meta_t.cls[] byte.
 * Locking the leaf per operation closes that, because every write targets
 * exactly one leaf entry.  It has to be per operation rather than per
 * transaction: a wide cursor visits many leaves and the per-CPU held[] stack has
 * only PT_MCS_POOL_SLOTS entries.
 *
 * Caller holds the cursor, so mm->pt_readers keeps an already-cached page from
 * being recycled under the descent; `stale` is what tells us we lost a race with
 * a detach, in which case we drop everything and report failure so the caller
 * re-descends. */
static pte_t *cursor_leaf_slot(mm_cursor_t *cur, vaddr_t addr, int create)
{
    /* This walk must START at guard_level: mm_addrspace_lock's descent fills
     * path[ROOT-1] .. path[guard_level] and nothing below, so starting at
     * guard_level - 1 would dereference an uninitialised pointer.  That mistake
     * hung the huge-page path, where wide cursors are routine, and it presented
     * as a smoke-mm-stress timeout with no self-deadlock report.
     *
     * At l == guard_level the cursor already holds that node's lock for its
     * whole lifetime, so taking it again is a non-reentrant self-deadlock -- the
     * per-CPU detector only notices after 2^26 spins.  Every level strictly
     * below is a different node and does need its own lock, which is what
     * excludes a peer writing the same leaf through a higher covering node. */
    for (int l = cur->guard_level; l > 0; l--) {
        pte_t *table = cur->path[l];
        int idx = arch_pt_vpn(addr, l);
        /* Allocate the child node with frame_alloc_nr(), which cannot reach
         * oom_try_reclaim() -- so it is safe to call while holding a node MCS
         * lock, and hoisting it above the lock keeps the allocation out of the
         * critical section as well.  The pre-check only decides whether
         * allocating is worth attempting; the authoritative state is the
         * re-read of table[idx] under the lock below, and every path that
         * finds the speculative page unnecessary frees it. */
        pte_t *next = NULL;
        if (create && !(table[idx] & PTE_V)) {
            next = (pte_t *)frame_alloc_nr();
            if (!next)
                return NULL;
        }
        /* The parent's lock covers both the entry read and the install: the
         * metadata write below is a read-modify-write on the parent's
         * nr_present/cls[], so reading the entry outside the lock would race
         * a peer installing the same child. */
        pt_meta_t *pm = (l == cur->guard_level) ? NULL : mm_pt_meta(table);
        if (pm)
            mcs_lock(pm);
        pte_t e = table[idx];
        if (!(e & PTE_V)) {
            if (!create) {
                if (next)
                    frame_free(next);
                if (pm)
                    mcs_unlock(pm);
                return NULL;
            }
            if (pm && pm->stale) {
                if (next)
                    frame_free(next);
                mcs_unlock(pm);
                return NULL;
            }
            if (!next) {
                if (pm)
                    mcs_unlock(pm);
                return NULL;
            }
            mm_pt_node_init(next, l - 1);
            table[idx] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
            mm_pt_note_present(table, l, idx,
                               MM_ST_CLS_BYTE(MM_ST_PT_NODE));
            if (pm)
                mcs_unlock(pm);
            cur->path[l - 1] = next;
            continue;
        }
        if (arch_pte_is_leaf(e)) {
            if (pm)
                mcs_unlock(pm);
            return NULL;      /* huge leaf covers more than one page */
        }
        cur->path[l - 1] = arch_pte_to_ptr(e);
        if (pm)
            mcs_unlock(pm);
    }

    /* When guard_level == 0 the covering node IS the leaf table and the cursor
     * already holds its lock, so there is nothing left to take. */
    if (cur->guard_level == 0)
        return &cur->path[0][arch_pt_vpn(addr, 0)];

    pte_t *leaf = cur->path[0];
    pt_meta_t *lm = mm_pt_meta(leaf);
    if (!lm)
        return NULL;
    mcs_lock(lm);
    if (lm->stale) {
        mcs_unlock(lm);
        return NULL;          /* detached underneath us; caller re-descends */
    }
    cur->leaf_meta = lm;
    return &leaf[arch_pt_vpn(addr, 0)];
}

/* Release the leaf lock taken by the matching cursor_leaf_slot().  Every
 * failure return of that function leaves the lock already released, so calling
 * this unconditionally after a non-NULL result is correct and idempotent. */
static void cursor_leaf_unlock(mm_cursor_t *cur)
{
    if (cur->leaf_meta) {
        mcs_unlock(cur->leaf_meta);
        cur->leaf_meta = NULL;
    }
}

static inline pte_t *cursor_leaf_table(const mm_cursor_t *cur)
{
    return cur->path[0];
}


/* ------------------------------------------------------------------ *
 * Segment annotation walk (P6)
 * ------------------------------------------------------------------ *
 * Annotate every EXISTING node entry covered by [start, end) with `seg`,
 * descending only into nodes that are already there.
 *
 * "Existing" is the whole trick.  Creating the path is mm's own job and it
 * already does that exactly once per mmap; this walk merely labels what the
 * path produced.  So the cost is the number of node entries the mapping's
 * path already contains -- three or four for a 3 GiB Sv39 mapping -- instead
 * of one write per page.  Annotating by leaf would have had to materialise
 * every leaf table in the range first, which is the cost that made eager
 * provisioning ship off (see mm/pt.h).
 *
 * Descending only into existing nodes is also what makes partial overlaps
 * fall out for free.  An entry inside [start, end) that is an absent leaf
 * stays absent and unannotated: it belongs to no mapping yet.  An entry that
 * is an existing node gets labelled, and so does everything under it -- which
 * is correct only because mmap has just created that node for this range.
 *
 * This paragraph described the design all along while the code did the
 * opposite: the walk carried a `provision` flag and mm_pt_annotate_seg()
 * passed 1, so it built the very nodes it was about to label.  See the comment
 * on the absent-entry skip below for what that cost once anonymous mappings
 * were annotated too.
 *
 * Locking matches cursor_leaf_slot(): the parent is locked around the entry
 * read and the segtab read-modify-write, because both live in the parent's
 * metadata.  No leaf lock is taken and none is needed -- this walk writes no
 * PTE.
 */
static int seg_annotate_rec(pte_t *table, int level, vaddr_t start,
                            vaddr_t end, mm_seg_t *seg, int depth)
{
    if (level < 1 || depth > ARCH_PT_ROOT_LEVEL)
        return 0;

    pt_meta_t *pm = mm_pt_meta(table);
    if (!pm)
        return 0;
    mcs_lock(pm);
    if (pm->stale) {
        mcs_unlock(pm);
        return -EAGAIN;
    }

    int i0 = arch_pt_vpn(start, level);
    int i1 = arch_pt_vpn(end - 1, level);
    int entries = arch_pt_level_entries(level);

    /* Descend into at most one child: a range that spans several entries at
     * this level means its children are absent (mmap does not pre-create
     * them), so there is nothing below to label.  Established by reading the
     * entries under this table's own lock. */
    int multi = (i0 != i1);
    pte_t only_child = 0;

    mm_segtab_t *st = pm->segtab;
    int annotated = 0;

    for (int i = i0; i <= i1 && i < entries; i++) {
        pte_t e = table[i];
        /* An absent entry is skipped, NOT provisioned.  This walk used to
         * build the intermediate node it wanted to label (provision=1), which
         * contradicted the contract stated at its own call site: "annotating
         * an untouched range must not bring page tables into existence just to
         * label them".  The contradiction was affordable while only file and
         * VMO mappings were annotated, because there are few of them and they
         * are small.  Anonymous mappings were then given segments too -- heap,
         * stack, every anonymous mmap -- and each one materialised a whole
         * page-table tree across its entire extent.  The guest ran out of
         * memory on the git stage and died with SIGSEGV, then panicked.
         *
         * The cost was never the walk; it was that the walk was turning a
         * cheap label into an unbounded allocation.  Coverage does not suffer:
         * mm_mmap_seg_label() re-applies the segment after the first fault has
         * built part of the path, which is exactly the case provisioning was
         * supposed to cover. */
        if (!(e & PTE_V) || arch_pte_is_leaf(e))
            continue;                       /* absent leaf or a real leaf */
        if (!st)
            st = segtab_attach_locked(pm);
        uint8_t slot = st ? segtab_slot(st, seg) : 0;
        if (!slot) {
            mm_seg_annot_lost[0]++;
            if (level < 8)
                mm_seg_full_lvl[level]++;
        }
        if (slot) {
            /* Name this segment alongside whatever the entry already names -- an
             * entry is much coarser than a mapping, so two of them can
             * legitimately share it.  Lookup picks between them by extent. */
            if (!segtab_entry_names(st, i, slot)) {
                if (!segtab_entry_name(st, i, slot)) {
                    /* Record the level here too.  It used to be recorded only
                     * on the shared-array-full branch above, which made the
                     * level histogram describe a different event than the
                     * counter it sat next to: mm_seg_full_lvl[] read
                     * [0,0,0] while mm_seg_annot_lost[1] read 4329.  The
                     * level is the whole question -- names lost at the root
                     * mean 7 mappings per GiB, names lost two levels down mean
                     * 7 per 4 KiB -- so a histogram that cannot tell them apart
                     * cannot tell whether the losses are coarse or fine. */
                    mm_seg_annot_lost[1]++;
                    if (level < 8)
                        mm_seg_full_lvl[level]++;
                } else {
                    annotated = 1;
                }
            }
        }
        /* No slot here -- either the attachment failed or this node page's
         * shared array is already at MM_SEGTAB_MAX -- and that is NOT a reason
         * to abandon the range.  An earlier version broke out of the loop,
         * which made the root
         * table's four slots a hard ceiling on the whole address space: once
         * they filled, every later mapping went unlabelled and every fault on
         * it had to fall back to the VMA.  Descend instead.  The child node
         * owns its own segtab with its own MM_SEGTAB_MAX free slots, and
         * mm_pt_lookup_seg_rcu() already keeps descending past a name that
         * does not cover the address -- so a name one level down is found
         * exactly as well as one here, and costs one more node walk. */
        if (i0 == i1)
            only_child = e;
    }
    mcs_unlock(pm);

    if (multi)
        return annotated;

    /* Exactly one node covers the whole range at this level: keep descending,
     * because the fault descent will stop at the DEEPEST annotated node and a
     * shallower annotation would be ambiguous for an address that a later
     * mapping splits off.
     *
     * With no child there is nothing to descend into, and the range is not
     * annotated at this level -- which used to be where a node got built. */
    if (!only_child)
        return annotated;

    int rc = seg_annotate_rec(arch_pte_to_ptr(only_child), level - 1,
                              start, end, seg, depth + 1);
    return rc < 0 ? rc : annotated;
}

/* Public entry: annotate [start, end) of `mm`.  Runs its own per-node locking
 * and touches no PTE, but it DOES read user PTEs, so it needs the same
 * read-side section a cursor establishes -- see below.
 *
 * An earlier version of this comment claimed it needed no read-side section
 * because mm->lock was held, and that was wrong.  mm->lock keeps a concurrent
 * unmapping of the MAPPING away; it does not stop a cursor on another CPU from
 * collapsing a subtree, and it does not stop that frame being recycled.  The
 * symptom was concrete: mm_pt_meta() validated the frame's flags, the frame was
 * recycled in between, and mcs_lock() was handed the recycled descriptor --
 * a fault at address 0xffffffff00000000 on the first file fault after boot.
 *
 * So the walk runs inside a cursor opened on the range.  It takes no PTE lock
 * of its own beyond the per-node locks it already takes, so it cannot deadlock
 * against the cursor's own descent, and the read-side section is what keeps the
 * descriptors it cached valid. */
int mm_pt_annotate_seg(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                       mm_seg_t *seg)
{
    if (!mm || !mm->pgdir || !seg || end <= start)
        return -EINVAL;
    if (!mm_pt_range_is_user(start, end))
        return -EFAULT;
    /* Only level >= 1 entries carry a segment; a range inside a single leaf
     * table cannot be represented, and mmap never produces one that matters
     * because the leaf table's own parent was annotated on the way down.
     *
     * The read-side section is entered directly rather than through a cursor:
     * mm_addrspace_lock() also CREATES the missing levels, which is right for
     * a fault and wrong here -- annotating an untouched range must not bring
     * page tables into existence just to label them. */
    mm_pt_read_enter(mm);
    int rc = seg_annotate_rec(mm->pgdir, ARCH_PT_ROOT_LEVEL, start, end, seg,
                              0);
    mm_pt_read_exit(mm);
    return rc;
}

/* Resolve the segment backing `addr`, by descending until an annotated node
 * is met.  Returns NULL for an anonymous mapping, which is the common case and
 * must stay cheap: the walk stops at the first node without a segtab.
 *
 * NULL is also the answer when the named segment does not actually COVER
 * `addr` -- see "WHERE A SEGMENT STOPS BEING AUTHORITATIVE" in mm/pt.h.  The
 * annotation is one node entry wide, a split boundary can land inside one, and
 * in that case the entry names a segment that does not describe this address.
 * Refusing is what keeps the fault path honest while the VMA is still there to
 * fall back on.
 *
 * Returns with a reference held (mm_seg_get), so the caller may drop mm->lock
 * and then mm_seg_put -- the same discipline mm_seg_find + mm_vma_get already
 * required, and for the same reason.
 *
 * Runs its own read-side section, so callers need no cursor -- and must NOT
 * pass one that would allocate page tables, because a lookup that creates the
 * path it is looking for reports coverage it manufactured. */
static mm_seg_t *mm_pt_lookup_seg_rcu(mm_struct_t *mm, vaddr_t addr);
mm_seg_t *mm_pt_lookup_seg(mm_struct_t *mm, vaddr_t addr)
{
    if (!mm || !mm->pgdir)
        return NULL;

    /* Read-side section, for the same reason the annotate walk needs one: the
     * descent below caches physical pointers, so a concurrent detach must not
     * be allowed to recycle them.  mm->lock does not provide that -- it does
     * not stop a cursor on another CPU from collapsing a subtree.  The counter
     * nests, so a caller that already holds a section is fine.
     *
     * Deliberately NOT mm_addrspace_lock(): this must not allocate page
     * tables.  A reader is allowed to find nothing, and a lookup that created
     * the path it was looking for would report coverage it manufactured. */
    mm_pt_read_enter(mm);
    mm_seg_t *s = mm_pt_lookup_seg_rcu(mm, addr);
    mm_pt_read_exit(mm);
    return s;
}

/* See the definitions, and the reason these counters exist at all, in mm/pt.h. */
uint64_t mm_seg_miss_why[MM_MW_COUNT];
uint64_t mm_seg_annot_lost[2];
uint64_t mm_seg_full_lvl[8];

/* Set by the lookup on every NULL return; the caller decides whether to count
 * it.  Counting inside the lookup itself would fold in the auditor's own
 * lookups, which fall through constantly and say nothing about faults -- the
 * first version of this counter did exactly that and reported 47650 misses for
 * a workload that had 1987. */
static int mm_seg_last_why;

static mm_seg_t *mm_pt_lookup_seg_rcu(mm_struct_t *mm, vaddr_t addr)
{
    pte_t *table = mm->pgdir;
    int saw_unnamed = 0, saw_extent = 0;
    for (int l = ARCH_PT_ROOT_LEVEL; l > 0; l--) {
        int idx = arch_pt_vpn(addr, l);
        pte_t e = table[idx];
        if (!(e & PTE_V)) {
            mm_seg_last_why = MM_MW_HOLE;
            return NULL;                   /* hole */
        }
        if (arch_pte_is_leaf(e)) {
            mm_seg_last_why = MM_MW_LEAF;
            return NULL;                   /* a huge leaf */
        }
        pt_meta_t *pm = mm_pt_meta(table);
        if (pm && pm->segtab) {
            /* An entry can name several segments -- it is much coarser than a
             * mapping, and neighbouring mappings routinely share one.  The
             * extent recorded in each is what tells them apart, so try them all
             * and take the one that actually covers this address. */
            mm_seg_t *hit = NULL;
            int ambiguous = 0;
            int named = 0;
            for (int k = 0; k < MM_SEGTAB_NAMES; k++) {
                uint8_t slot = segtab_slot_get(pm->segtab, idx, k);
                if (!slot)
                    continue;
                named = 1;
                mm_seg_t *cand = segtab_seg(pm->segtab, slot);
                if (!cand || addr < cand->start || addr >= cand->end)
                    continue;               /* named, but not about this addr */
                if (hit) {
                    /* Two live mappings both claim this address.  That cannot
                     * happen with a consistent VMA list, so rather than pick
                     * one and read the wrong page, decline and let the caller
                     * fall back. */
                    ambiguous = 1;
                    break;
                }
                hit = cand;
            }
            if (ambiguous) {
                mm_seg_last_why = MM_MW_AMBIG;
                mm_seg_put(hit);
                return NULL;
            }
            if (hit)
                return mm_seg_get(hit);
            if (named)
                saw_extent = 1;
            else
                saw_unnamed = 1;
            /* Named but none about this address: keep descending, a deeper
             * node may still carry the right one. */
        }
        table = arch_pte_to_ptr(e);
    }
    mm_seg_last_why = saw_extent ? MM_MW_EXTENT
                                 : (saw_unnamed ? MM_MW_UNNAMED : MM_MW_BOTTOM);
    return NULL;
}

/* ---- P6 shadow check -------------------------------------------------
 *
 * The whole argument for retiring the VMA is that the page tables already
 * carry enough to answer a fault.  For the backing object that argument had
 * never been tested: the segment table was justified by construction, not by
 * measurement.  So ask it, on every file and VMO fault, what it would have
 * said -- and count how often that matches what the VMA actually said.
 *
 * A non-zero mm_seg_shadow_disagree is a real defect: the segment would have
 * made the fault read the wrong page of the wrong file.  A large
 * mm_seg_shadow_miss is not a defect, it is the remaining work, measured.
 */
uint64_t mm_seg_shadow_agree;
uint64_t mm_seg_shadow_disagree;
uint64_t mm_seg_shadow_miss;

/* Faults actually DISPATCHED from the segment rather than fallen back to the
 * VMA.  Without this the dispatch change is unverifiable from the outside: seg_ok
 * only says the segment and the VMA agreed, not that the segment was obeyed.
 * A non-zero disagree with a zero here would mean the fallback was taken every
 * time and the change did nothing. */
uint64_t mm_seg_dispatch_seg;
uint64_t mm_seg_dispatch_fallback;

int mm_pt_shadow_seg(mm_struct_t *mm, vaddr_t addr, uint8_t kind,
                     uint64_t off, int shared, mm_seg_t **found)
{
    if (found)
        *found = NULL;
    if (!mm || !mm->pgdir)
        return -1;

    /* No cursor here, deliberately: a measurement must not change the thing it
     * measures.  mm_addrspace_lock() would materialise the page-table path for
     * this address first, and the segment table would then be credited with
     * coverage the lookup itself created. */
    mm_seg_last_why = MM_MW_BOTTOM;
    mm_seg_t *s = mm_pt_lookup_seg(mm, addr);
    int verdict;
    if (!s) {
        mm_seg_miss_why[mm_seg_last_why]++;
        verdict = -1;
    } else if (mm_seg_kind(s) == kind && mm_seg_shared(s) == (shared ? 1 : 0) &&
               s->backing_offset + (addr - s->start) == off) {
        verdict = 1;
    } else {
        verdict = 0;
        /* Hand the offending record to the caller, which resolved the address
         * through mm->mmap and can therefore print the two next to each other.
         * pt.c deliberately does not walk that list: the whole point of the
         * comparison is to be able to say "the page tables name a different
         * mapping than the list does", and a checker that walked the list
         * itself would collapse the two sides into one.  Capped, because a
         * systematic disagreement would otherwise print once per fault for
         * the whole workload. */
        if (found && mm_seg_shadow_disagree < MM_SEG_SHADOW_REPORT) {
            *found = s; /* reference transferred to the caller */
            s = NULL;
        }
    }
    mm_seg_put(s); /* NULL-safe, and a no-op once the reference moved out */

    if (verdict > 0)
        mm_seg_shadow_agree++;
    else if (verdict == 0)
        mm_seg_shadow_disagree++;
    else
        mm_seg_shadow_miss++;
    return verdict;
}

static void mm_pt_node_forget_seg(pte_t *table, int level, int idx,
                                  mm_seg_t *only);

/* Drop the annotations covering [start, end).
 *
 * The unmap paths in mm.c only collapse nodes that hold no leaves, so a
 * mapping that was never faulted -- the case a segment exists precisely to
 * serve -- leaves its labels behind when it is unmapped.  They are not
 * reachable afterwards (there is no VMA to fault into), but they keep the
 * segment's vnode reference alive and would be handed to a later mapping that
 * reuses the same node entry.  Walking the range to clear them costs one pass
 * over the mapping's own path, and only for ranges that were annotated.
 *
 * Same descent rules as seg_annotate_rec(): only into nodes that exist, and
 * only as deep as a single node covers the range. */
static void seg_unannotate_rec(pte_t *table, int level, vaddr_t start,
                               vaddr_t end, int depth, mm_seg_t *only)
{
    if (level < 1 || depth > ARCH_PT_ROOT_LEVEL)
        return;

    pt_meta_t *pm = mm_pt_meta(table);
    if (!pm)
        return;
    mcs_lock(pm);
    if (pm->stale) {
        mcs_unlock(pm);
        return;
    }

    int i0 = arch_pt_vpn(start, level);
    int i1 = arch_pt_vpn(end - 1, level);
    int entries = arch_pt_level_entries(level);
    int multi = (i0 != i1);
    pte_t only_child = 0;

    for (int i = i0; i <= i1 && i < entries; i++) {
        pte_t e = table[i];
        if (!(e & PTE_V) || arch_pte_is_leaf(e))
            continue;
        if (i0 == i1)
            only_child = e;
    }
    /* Clearing happens under this table's own lock, which is the lock the
     * annotation for these entries is protected by.  `only` names the segment
     * being dropped: an entry may also carry a neighbouring mapping's name (see
     * "Entry index packing"), and dropping that one too would cost coverage
     * for a mapping that is still perfectly well described. */
    for (int i = i0; i <= i1 && i < entries; i++)
        mm_pt_node_forget_seg(table, level, i, only);
    mcs_unlock(pm);

    if (multi || !only_child)
        return;
    seg_unannotate_rec(arch_pte_to_ptr(only_child), level - 1, start, end,
                       depth + 1, only);
}

void mm_pt_unannotate_seg(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                          mm_seg_t *only)
{
    if (!mm || !mm->pgdir || end <= start)
        return;
    if (!mm_pt_range_is_user(start, end))
        return;
    /* Same read-side section as the annotate walk, for the same reason -- and
     * for one more: this runs from munmap, where a cursor would allocate the
     * very page tables being torn down. */
    mm_pt_read_enter(mm);
    seg_unannotate_rec(mm->pgdir, ARCH_PT_ROOT_LEVEL, start, end, 0, only);
    mm_pt_read_exit(mm);
}

/* Drop names from a single node entry and release any segment the whole table
 * has stopped naming.
 *
 * `only` selects what goes: NULL drops every name on the entry (the entry
 * itself is going away, so nothing it named is still described), a segment
 * drops just that mapping's name (its neighbours keep theirs -- see "Entry
 * index packing").  Caller holds the entry's parent node lock.
 *
 * Releasing the slots nothing points at is not an optimisation.  `n` is the
 * number of segments this table holds a reference FOR, so a slot no entry
 * names is a reference with no path to its release: a leak that no counter in
 * the audit can see. */
static void segtab_entry_forget(mm_segtab_t *st, int idx, mm_seg_t *only)
{
    struct mm_segarr *a = st->arr;
    if (!a)
        return;
    int named_here = 0;

    if (only) {
        /* Find the slot naming `only` and blank just that slot. */
        uint8_t slot = 0;
        for (uint8_t j = 0; j < a->n; j++) {
            if (a->seg[j] == only) {
                slot = (uint8_t)(j + 1);
                break;
            }
        }
        if (!slot)
            return;                        /* this entry never named it */
        for (int k = 0; k < MM_SEGTAB_NAMES; k++) {
            if (segtab_slot_get(st, idx, k) == slot) {
                segtab_slot_set(st, idx, k, 0);
                named_here = 1;
            }
        }
        if (!named_here)
            return;                        /* this entry never named it */
    } else {
        for (int k = 0; k < MM_SEGTAB_NAMES; k++)
            segtab_slot_set(st, idx, k, 0);
    }

    /* Compact: release every slot no entry names, then renumber what is left so
     * the packed slots still agree with the array.  Done in one pass over the
     * used set rather than slot-by-slot, because removing slot k renumbers every
     * slot above it and a single-pass removal would then test stale numbers.
     *
     * `used` is a bitmap rather than a bitmask: MM_SEGTAB_MAX is 255, so a
     * uint16_t would silently alias slots 1 and 17 and release a segment some
     * live entry still names. */
    uint8_t used[(MM_SEGTAB_MAX + 7) / 8];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < MM_PT_META_ENTRIES; i++)
        for (int k = 0; k < MM_SEGTAB_NAMES; k++) {
            uint8_t v = segtab_slot_get(st, i, k);
            if (v)
                used[(v - 1) >> 3] |= (uint8_t)(1u << ((v - 1) & 7));
        }

    /* These counters are `int`, not `uint8_t`, and that is load-bearing.
     * MM_SEGTAB_MAX is 255, so a uint8_t loop bounded by `s <= a->n` wraps to 0
     * the moment the array fills and spins forever.  It never could at the old
     * cap of 8, which is why the bound stayed safe right up until the array
     * grew: the guest hung on the first git stage. */
    uint8_t remap[MM_SEGTAB_MAX + 1];
    memset(remap, 0, sizeof(remap));
    int w = 0;
    int any = 0;
    for (int s = 1; s <= a->n; s++) {
        if (used[(s - 1) >> 3] & (uint8_t)(1u << ((s - 1) & 7))) {
            a->seg[w] = a->seg[s - 1];
            remap[s] = (uint8_t)++w;
            any = 1;
        } else {
            mm_seg_put(a->seg[s - 1]);
        }
    }
    for (int s = w; s < a->n; s++)
        a->seg[s] = NULL;
    a->n = (uint8_t)w;

    if (any) {
        for (int i = 0; i < MM_PT_META_ENTRIES; i++)
            for (int k = 0; k < MM_SEGTAB_NAMES; k++) {
                uint8_t v = segtab_slot_get(st, i, k);
                if (v)
                    segtab_slot_set(st, i, k, remap[v]);
            }
    }
}

/* Clear the segment index of a single entry, if any.  Used when an entry
 * stops being a node -- the unmap path that collapses a child table back into
 * a leaf must not leave a stale index behind, or the next lookup would hand
 * out a segment for a page that no longer belongs to that mapping.
 * Caller holds the entry's parent node lock. */
void mm_pt_node_clear_seg(pte_t *table, int level, int idx)
{
    pt_meta_t *pm = mm_pt_meta(table);
    if (!pm || !pm->segtab || level < 1)
        return;
    if (idx < 0 || idx >= MM_PT_META_ENTRIES)
        return;

    segtab_entry_forget(pm->segtab, idx, NULL);
    segtab_maybe_free_locked(pm);
}

/* Drop one mapping's name from an entry, leaving any co-tenant's name alone.
 * Caller holds the entry's parent node lock. */
static void mm_pt_node_forget_seg(pte_t *table, int level, int idx,
                                  mm_seg_t *only)
{
    pt_meta_t *pm = mm_pt_meta(table);
    if (!pm || !pm->segtab || level < 1 || !only)
        return;
    if (idx < 0 || idx >= MM_PT_META_ENTRIES)
        return;

    segtab_entry_forget(pm->segtab, idx, only);
    segtab_maybe_free_locked(pm);
}

/* Release the segment table if it no longer names anything, so an address
 * space that unmapped its file mapping stops paying for the page.  An empty
 * segtab is not merely wasteful: `n` is the count of segments whose references
 * this table owns, so keeping an empty one alive would hide a leak by never
 * giving the last reference anywhere to go. */
static void segtab_maybe_free_locked(pt_meta_t *pm)
{
    mm_segtab_t *st = pm->segtab;
    if (!st || !st->arr || st->arr->n != 0)
        return;
    segtab_detach_locked(pm);
}

/* ------------------------------------------------------------------ *
 * Cursor
 * ------------------------------------------------------------------ */
int mm_addrspace_lock(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                      mm_cursor_t *cur)
{
    if (!mm || !mm->pgdir || !cur)
        return -EINVAL;
    if (end <= start)
        return -EINVAL;
    if (!mm_pt_range_is_user(start, end))
        return -EFAULT;

    int level = pt_covering_level(start, end);

    cur->mm = NULL;
    cur->locked = 0;
    cur->leaf_meta = NULL;
    cur->start = start;
    cur->end = end;
    cur->guard_level = level;
    cur->path[ARCH_PT_ROOT_LEVEL] = mm->pgdir;

    /* Read-side section: from here on the descent caches physical pointers,
     * so a concurrent detach must be prevented from recycling them.  Paired
     * in mm_cursor_unlock, including every error return below. */
    cur->in_read_side = 1;
    mm_pt_read_enter(mm);

    pte_t *table = mm->pgdir;

    /* Walk down, allocating missing levels.  A first-touch fault routinely
     * arrives with no intermediate node at all, so this must be able to
     * create the path -- and it must do so under the parent's lock, which is
     * why RW (which requires a fully populated table) is not an option here.
     * On a stale node we drop everything and retry, because a subtree may
     * have been detached while we were descending. */
    for (int attempt = 0; attempt < 8; attempt++) {
        int retry = 0;
        table = mm->pgdir;
        cur->path[ARCH_PT_ROOT_LEVEL] = table;

        for (int l = ARCH_PT_ROOT_LEVEL; l > level; l--) {
            int idx = arch_pt_vpn(start, l);
            pte_t e = table[idx];

            if ((e & PTE_V) && !arch_pte_is_leaf(e)) {
                table = arch_pte_to_ptr(e);
                cur->path[l - 1] = table;
                continue;
            }

            if ((e & PTE_V) && arch_pte_is_leaf(e)) {
                /* A larger-than-needed leaf already covers the range.  The
                 * caller must demote it (mm_demote_huge_page) before a
                 * transaction can address individual pages inside it. */
                return 1;
            }

            /* Hoisted for the same reason as cursor_leaf_slot(), and
             * non-reclaiming for the same reason -- which matters most HERE:
             * mm_mmap() reaches this function while holding mm->lock, and that
             * lock is a spin_lock_irqsave, so a reclaiming allocation on this
             * path is a sleep with interrupts off.  The unlocked e read at the
             * top of this loop body is only the heuristic; the re-read under
             * the lock is authoritative. */
            pte_t *next = NULL;
            if (!(e & PTE_V)) {
                next = (pte_t *)frame_alloc_nr();
                if (!next) {
                    mm_pt_read_exit(mm);
                    cur->in_read_side = 0;
                    return -ENOMEM;
                }
            }

            pt_meta_t *pm = mm_pt_meta(table);
            if (!pm && mm_pt_node_init(table, l) < 0) {
                if (next)
                    frame_free(next);
                mm_pt_read_exit(mm);
                cur->in_read_side = 0;
                return -ENOMEM;
            }
            pm = mm_pt_meta(table);
            if (!pm) {
                if (next)
                    frame_free(next);
                mm_pt_read_exit(mm);
                cur->in_read_side = 0;
                return -ENOMEM;
            }

            mcs_lock(pm);
            if (pm->stale) {
                mcs_unlock(pm);
                a20_perf_count(A20_PERF_MM_CURSOR_STALE_RETRY);
                retry = 1;
                break;
            }

            /* Re-read under the lock: another cursor may have created it. */
            e = table[idx];
            if ((e & PTE_V) && !arch_pte_is_leaf(e)) {
                if (next)
                    frame_free(next);
                mcs_unlock(pm);
                table = arch_pte_to_ptr(e);
                cur->path[l - 1] = table;
                continue;
            }
            if ((e & PTE_V) && arch_pte_is_leaf(e)) {
                if (next)
                    frame_free(next);
                mcs_unlock(pm);
                mm_pt_read_exit(mm);
                cur->in_read_side = 0;
                return 1;
            }

            if (!next) {
                mcs_unlock(pm);
                mm_pt_read_exit(mm);
                cur->in_read_side = 0;
                return -ENOMEM;
            }
            mm_pt_node_init(next, l - 1);
            table[idx] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
            mm_pt_note_present(table, l, idx,
                               MM_ST_CLS_BYTE(MM_ST_PT_NODE));
            mcs_unlock(pm);

            table = next;
            cur->path[l - 1] = table;
        }
        if (!retry)
            break;
        if (attempt == 7) {
            mm_pt_read_exit(mm);
            cur->in_read_side = 0;
            return -EAGAIN;
        }
    }

    pt_meta_t *m = mm_pt_meta(table);
    if (!m && mm_pt_node_init(table, level) < 0) {
        mm_pt_read_exit(mm);
        cur->in_read_side = 0;
        return -ENOMEM;
    }
    m = mm_pt_meta(table);
    if (!m) {
        mm_pt_read_exit(mm);
        cur->in_read_side = 0;
        return -ENOMEM;
    }

    cur->lock_base_depth = (int)g_pt_mcs_pool[pt_cpu()].depth;
    mcs_lock(m);
    if (m->stale) {
        mcs_unlock(m);
        cur->lock_base_depth = 0;
        mm_pt_read_exit(mm);
        cur->in_read_side = 0;
        return -EAGAIN;      /* racing a subtree detach; caller retries */
    }

    /* The covering node's lock is range-level mutual exclusion, and it is NOT
     * sufficient on its own.  It used to be documented here as the unit of
     * writer exclusion -- on the reasoning that "every other cursor that could
     * touch that path must first acquire this same node" -- and that is false:
     * a peer whose covering node is an ancestor or a descendant holds a
     * different lock.  A wide cursor (covering level 2) and a single-page
     * cursor (covering level 0) inside it would hold disjoint locks while
     * writing the same leaf PTE and the same pt_meta_t.cls[] byte.  That is why
     * cursor_leaf_slot() takes the leaf's own lock per operation; see the
     * locking contract in mm/pt.h.  Two cursors conflict exactly when they
     * touch the same leaf table, which is what preserves the paper's semantics:
     * disjoint ranges run in parallel, overlapping ranges serialise. */
    cur->mm = mm;
    cur->locked = 1;
    a20_perf_count(A20_PERF_MM_CURSOR_OPEN);
    return 0;
}

void mm_cursor_unlock(mm_cursor_t *cur)
{
    if (!cur || !cur->locked || !cur->mm)
        return;
    /* A leaked per-operation leaf lock would otherwise stay held for the rest
     * of the cursor's life; the unwind below cannot pop it because it is not
     * tracked in the per-CPU held[] stack. */
    cursor_leaf_unlock(cur);
    /* Release every lock taken since the cursor opened, in reverse.  The
     * per-CPU held[] stack is the record; the cursor only remembers the
     * depth it started at, so a DFS of any width unwinds correctly. */
    pt_mcs_pool_t *pool = &g_pt_mcs_pool[pt_cpu()];
    while (pool->depth > (uint32_t)cur->lock_base_depth) {
        pt_meta_t *m = pool->held[pool->depth - 1];
        if (m) {
            mcs_unlock(m);          /* pops the depth slot itself */
        } else {
            pool->depth--;          /* no node recorded: pop the slot */
        }
    }
    if (cur->in_read_side) {
        mm_pt_read_exit(cur->mm);
        cur->in_read_side = 0;
    }
    cur->locked = 0;
    cur->mm = NULL;
}

static int cursor_span_ok(const mm_cursor_t *cur, vaddr_t addr)
{
    return cur && cur->locked && cur->mm &&
           addr >= cur->start && addr < cur->end;
}

uint8_t mm_pt_prot_bits(pte_t flags)
{
    uint8_t prot = 0;
    if (flags & PTE_R) prot |= MM_ST_PROT_R;
    if (flags & PTE_W) prot |= MM_ST_PROT_W;
    if (flags & PTE_X) prot |= MM_ST_PROT_X;
    return prot;
}

/* Compose the status byte a mapping with these PTE flags must carry. */
static inline uint8_t status_byte(uint8_t cls, pte_t flags)
{
    return (uint8_t)(MM_ST_CLS_BYTE(cls) |
                     (flags & PTE_COW ? MM_ST_COW_BIT : 0) |
                     mm_pt_prot_bits(flags));
}

/*
 * Install a mapping and hand back the frame it displaced WITHOUT releasing
 * it.  The COW path needs this: the old reference must survive until after
 * the remote TLB shootdown, and the rc==1 branch runs with pfa.lock already
 * held, so a frame_put() inside here would deadlock against itself.
 */
int mm_cursor_replace(mm_cursor_t *cur, vaddr_t addr, paddr_t pa, pte_t flags,
                      uint8_t cls, paddr_t *old_pa_out)
{
    if (old_pa_out)
        *old_pa_out = 0;
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;
    if (cls >= MM_ST_CLASS_MAX)
        return -EINVAL;

    pte_t *pte = cursor_leaf_slot(cur, addr, 1);
    if (!pte)
        return -ENOMEM;

    pte_t old = *pte;
    pte_t old_pa = 0;
    int had_old = 0;
#ifdef CONFIG_SWAP
    if (!pte_is_swap(old))
#endif
    if ((old & PTE_V) && arch_pte_is_leaf(old)) {
        old_pa = arch_pte_addr(old);
        had_old = 1;
    }

    /* Executable mappings may be populated through PAGE_OFFSET before being
     * installed at their user VA; synchronise the I-cache before the PTE
     * becomes visible. */
    if (flags & PTE_X) {
        pfn_t pfn = phys_to_pfn(pa);
        if (pfn_valid(pfn))
            arch_flush_icache_range(pfn_to_virt(pfn), PAGE_SIZE);
    }
    *pte = arch_pte_leaf(pa, flags);
    mm_pt_note_present(cursor_leaf_table(cur), 0, arch_pt_vpn(addr, 0),
                       status_byte(cls, flags));

    if (had_old && old_pa_out)
        *old_pa_out = old_pa;
    cursor_leaf_unlock(cur);
    return 0;
}

int mm_cursor_map(mm_cursor_t *cur, vaddr_t addr, paddr_t pa, pte_t flags,
                  uint8_t cls)
{
    paddr_t old_pa = 0;
    int r = mm_cursor_replace(cur, addr, pa, flags, cls, &old_pa);
    if (r < 0)
        return r;
    if (old_pa && old_pa != pa)
        frame_put(phys_to_pfn(old_pa));
    return 0;
}

int mm_cursor_unmap(mm_cursor_t *cur, vaddr_t addr)
{
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;

    /* Descend for THIS address.  cursor_leaf_table() on its own returns whatever
     * path[0] a previous operation left behind, which is the right leaf table
     * only while the whole cursor range sits inside one. */
    pte_t *pte = cursor_leaf_slot(cur, addr, 0);
    if (!pte)
        return 0;               /* no leaf table on this path: nothing mapped */
    pte_t *table = cursor_leaf_table(cur);
    int idx = arch_pt_vpn(addr, 0);

#ifdef CONFIG_SWAP
    if (pte_is_swap(*pte)) {
        swap_entry_t entry = pte_to_swp_entry(*pte);
        *pte = 0;
        mm_pt_note_absent(table, 0, idx);
        swap_free(entry);
        cursor_leaf_unlock(cur);
        return 1;
    }
#endif
    if (!(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) {
        /* A range provisioned by mm_pt_provision_anon() but never faulted has
         * no PTE, only a status mark.  Drop it here: leaving it behind would
         * let a later fault in the munmapped range read a stale "reserved"
         * status and silently map a page instead of failing. */
        if (MM_ST_GET_CLASS(mm_pt_peek(table, 0, idx)) == MM_ST_ANON_VIRT)
            mm_pt_note_absent(table, 0, idx);
        cursor_leaf_unlock(cur);
        return 0;
    }

    pte_t old = *pte;
    *pte = 0;
    mm_pt_note_absent(table, 0, idx);
    frame_put(phys_to_pfn(arch_pte_addr(old)));
    cursor_leaf_unlock(cur);
    return 1;
}

int mm_cursor_mark(mm_cursor_t *cur, vaddr_t addr, uint8_t cls)
{
    return mm_cursor_mark_prot(cur, addr, cls, 0);
}

/*
 * Refresh the permissions recorded in the per-PTE status for one leaf.
 *
 * mprotect() rewrites permissions through the PTE.  For a page that was
 * reserved but never faulted there is no PTE, so the status would keep the
 * permissions the range was created with and a later fault would install
 * those instead of the ones mprotect was asked for.
 *
 * A PRESENT page needs this too, which this function used to get wrong.  The
 * old comment claimed "its effective permissions live in the PTE, which
 * mprotect already updated" -- true of the MMU, but the status byte is the
 * authority the auditor checks and that a status-driven fault reads, so
 * leaving it stale makes the two representations disagree.  mm_pt_audit_all()
 * measured exactly that on a real workload: `prot=5` after vim and gcc had
 * both run mprotect.  Silently, because nothing consumed the counter.
 *
 * So the rule is uniform: whatever the PTE ends up carrying, the status must
 * carry too.  mprotect calls this for every leaf it rewrites, present or not.
 */
int mm_pt_refresh_leaf_prot(pte_t *table, int idx, pte_t ptef)
{
    if (!table)
        return -EINVAL;
    pt_meta_t *m = mm_pt_meta(table);
    if (!m)
        return 0;
    uint8_t *slot = cls_slot(m, idx);
    if (!slot)
        return 0;
    uint8_t cls = MM_ST_GET_CLASS(*slot);
    /* Only classes that describe a mapping the caller may re-protect.  A
     * PT_NODE slot is not a mapping, and INVALID has nothing to describe. */
    if (cls == MM_ST_INVALID || cls == MM_ST_PT_NODE)
        return 0;
    *slot = (uint8_t)((*slot & (uint8_t)~MM_ST_PROT_MASK) |
                      mm_pt_prot_bits(ptef));
    return 0;
}

/*
 * MM_AS_ANON_PROVISION -- eagerly mark an anonymous range as reserved.
 *
 * Mirrors the paper's on-demand paging (SS4.3): mmap records, per PTE, that
 * the range is "virtually allocated" and what its permissions are, so the
 * later fault can map a backing frame from that status alone instead of
 * re-deriving the mapping from a VMA.  The paper pays for the page-table
 * pages up front here, which is why its mmap is slightly slower than Linux's
 * while mmap-PF is faster (SS6.2).
 *
 * Each page is marked under its own cursor: mm_cursor_mark_prot() reads the
 * leaf table from cur->path[0], which the descent in mm_addrspace_lock()
 * only fills when the covering level is 0, so a wider lock range would leave
 * that slot stale.  Provisioning one leaf table's worth at a time is not
 * possible without widening that contract, so the cost is one descent per
 * page -- paid once at mmap, and skipped entirely for ranges above
 * MM_ANON_PROVISION_MAX_PAGES, which keep the VMA-based fault path.
 */
/*
 * Benchmark-only override for the eager-provisioning cap.
 *
 * MM_ANON_PROVISION_MAX_PAGES is a compile-time constant, so an ON/OFF A/B used
 * to need a full rebuild per arm.  On a shared host the load drifts *between*
 * arms, which is larger than the effect being measured (docs 10.12), so the
 * arms have to be interleaved -- and interleaving is impossible if switching
 * requires a rebuild.  a20.anonprov=<pages> makes it switchable at boot so one
 * build can run ON,OFF,ON,OFF,... against an identical kernel.
 *
 * Default is unchanged (MM_ANON_PROVISION_MAX_PAGES).  a20.anonprov=0 disables
 * provisioning entirely, which is the other arm of the experiment.
 */
/* Default cap.  Off unless a build asks for it via
 * -DCONFIG_ANON_PROV_DEFAULT=<n>; a20.anonprov=<n> overrides at boot, which is
 * how riscv64 drives the A/B.  x86_64 gets no command line at all, so its
 * experiments are built with different defaults and interleaved. */


void mm_pt_anon_prov_init(void)
{
    const char *cmdline = bootargs_get();
    if (!cmdline)
        return;

    const size_t klen = sizeof(MM_ANON_PROV_KEY) - 1;
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;

        if ((size_t)(tok_end - p) > klen && !strncmp(p, MM_ANON_PROV_KEY, klen) &&
            p[klen] == '=') {
            uint32_t v = 0;
            const char *q = p + klen + 1;
            int ok = (q < tok_end);
            for (; q < tok_end; q++) {
                if (*q < '0' || *q > '9') { ok = 0; break; }
                v = v * 10 + (uint32_t)(*q - '0');
            }
            if (ok)
                g_anon_prov_max = v;
            else
                kwarn("[PT] bad %s value, keeping %u (0 = off)\n",
                      MM_ANON_PROV_KEY, g_anon_prov_max);
        }
        p = tok_end;
    }
}

int mm_pt_provision_anon(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                         pte_t flags)
{
    if (!mm || !mm->pgdir || end <= start)
        return -EINVAL;
    if (!mm_pt_range_is_user(start, end))
        return -EFAULT;

    vaddr_t span = end - start;
    if (span / PAGE_SIZE > mm_pt_anon_prov_max())
        return 0;   /* too large to provision eagerly; VMA path still correct */

    uint8_t byte = status_byte(MM_ST_ANON_VIRT, flags);
    uint64_t marked = 0;

    /*
     * One cursor per leaf table, not per page.  A single-page range makes
     * mm_addrspace_lock() descend to covering level 0, so cur->path[0] is the
     * leaf table -- and the lock it takes IS that table's lock, so every entry
     * in the table is already excluded from concurrent writers.  The paper
     * likewise locks the covering PT page once for the whole range; opening a
     * fresh cursor per page instead cost 512 full round trips for a 2 MiB
     * mapping, and that is what made eager provisioning show up as a ~1.5x
     * mmap regression.
     */
    const vaddr_t chunk_bytes = ((vaddr_t)1 << ARCH_PT_BITS) * PAGE_SIZE;

    for (vaddr_t base = start & ~(chunk_bytes - 1); base < end;
         base += chunk_bytes) {
        vaddr_t chunk_end = base + chunk_bytes;
        if (chunk_end <= base)
            break;                       /* address-space wrap */
        vaddr_t lo = base > start ? base : start;
        vaddr_t hi = chunk_end < end ? chunk_end : end;

        vaddr_t anchor = lo & ~(vaddr_t)(PAGE_SIZE - 1);
        mm_cursor_t cur;
        int r = mm_addrspace_lock(mm, anchor, anchor + PAGE_SIZE, &cur);
        if (r < 0)
            return r;
        if (r > 0) {
            /* A larger leaf already covers this address.  Leave it: the
             * status belongs to that mapping, not to this reservation. */
            mm_cursor_unlock(&cur);
            continue;
        }

        pte_t *table = cur.path[0];
        pt_meta_t *m = mm_pt_meta(table);
        for (vaddr_t va = lo; va < hi; va += PAGE_SIZE) {
            int idx = arch_pt_vpn(va, 0);
            if (table[idx] & PTE_V)
                continue;               /* a fault won the race; already mapped */
            pt_note_present_meta(m, idx, byte);
            marked++;
        }
        mm_cursor_unlock(&cur);
    }
    /* One atomic add for the whole range: a per-page counter would cost an
     * atomic read-modify-write per page and dominate the loop. */
    a20_perf_add(A20_PERF_MM_ANON_PROVISIONED, marked);
    return 0;
}

/*
 * MM_AS_ANON_PROVISION: mark an unmapped leaf as reserved-but-not-backed,
 * recording the permissions the fault will later need.
 *
 * The paper stores exactly this in the per-PTE metadata at mmap time
 * (on-demand paging, §4.3): the range is marked "virtually allocated" with
 * its access permissions, and the fault handler then maps the backing frame
 * using the permissions from the metadata rather than re-deriving them from
 * a VMA.  This is also why the paper's mmap is slower than Linux's -- it
 * allocates and initialises page table pages up front (§6.2) -- while mmap-PF
 * is faster, because the fault stops touching VMA state entirely.
 */
int mm_cursor_mark_prot(mm_cursor_t *cur, vaddr_t addr, uint8_t cls,
                        pte_t flags)
{
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;
    if (cls >= MM_ST_CLASS_MAX)
        return -EINVAL;

    /* create=1: marking a reserved-but-unfaulted page is exactly the case
     * where the leaf table may not exist yet. */
    if (!cursor_leaf_slot(cur, addr, 1))
        return -ENOMEM;
    pte_t *table = cursor_leaf_table(cur);
    int idx = arch_pt_vpn(addr, 0);
    if (table[idx] & PTE_V) {
        cursor_leaf_unlock(cur);
        return -EEXIST;
    }
    mm_pt_note_present(table, 0, idx, status_byte(cls, flags));
    cursor_leaf_unlock(cur);
    return 0;
}

/* Test a per-entry safety bit through a cursor, honouring the cursor's span.
 * Lets a caller that already holds the covering node's lock ask "is this
 * entry userfaultfd-registered / fault-around-suppressed" without descending
 * a second time. */
int mm_cursor_safe_test(mm_cursor_t *cur, vaddr_t addr, unsigned flags)
{
    if (!cur || !flags || !cursor_span_ok(cur, addr))
        return 0;
    if (!cursor_leaf_slot(cur, addr, 0))
        return 0;
    int r = mm_pt_safe_test(cursor_leaf_table(cur), 0, arch_pt_vpn(addr, 0),
                            flags);
    cursor_leaf_unlock(cur);
    return r;
}

int mm_cursor_query(mm_cursor_t *cur, vaddr_t addr, uint8_t *cls_out,
                    paddr_t *pa_out)
{
    if (cls_out)
        *cls_out = MM_ST_CLS_BYTE(MM_ST_INVALID);
    if (pa_out)
        *pa_out = 0;
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;

    /* Descend for THIS address; see mm_cursor_unmap(). */
    pte_t *slot = cursor_leaf_slot(cur, addr, 0);
    if (!slot)
        return 0;               /* no leaf table here, or a larger leaf covers it */
    pte_t *table = cursor_leaf_table(cur);
    int idx = arch_pt_vpn(addr, 0);
    pte_t pte = table[idx];

#ifdef CONFIG_SWAP
    if (pte_is_swap(pte)) {
        if (cls_out)
            *cls_out = MM_ST_CLS_BYTE(MM_ST_SWAPPED) |
                       mm_pt_prot_bits(arch_pte_flags(pte));
        cursor_leaf_unlock(cur);
        return 1;
    }
#endif
    if (!(pte & PTE_V)) {
        /* An absent leaf is not "no information".  For a range that
         * mm_pt_provision_anon() reserved at mmap time it is exactly the
         * PrivateAnon state a status-driven fault is meant to act on, so the
         * recorded class (and the permissions the fault will install) must be
         * reported.  Still return 0: that means "not currently mapped", which
         * is still true, and the caller distinguishes the two. */
        uint8_t byte = mm_pt_peek(table, 0, idx);
        if (cls_out && MM_ST_GET_CLASS(byte) != MM_ST_INVALID)
            *cls_out = byte;
        cursor_leaf_unlock(cur);
        return 0;
    }
    if (!arch_pte_is_leaf(pte)) {
        cursor_leaf_unlock(cur);
        return 0;
    }

    /* The metadata is authoritative for the class; the PTE is authoritative
     * for the frame and for the effective permission bits. */
    uint8_t byte = mm_pt_peek(table, 0, idx);
    if (MM_ST_GET_CLASS(byte) == MM_ST_INVALID)
        byte = MM_ST_CLS_BYTE(MM_ST_ANON_MAPPED);
    byte = (uint8_t)((byte & (uint8_t)~MM_ST_PROT_MASK) |
                     mm_pt_prot_bits(pte));

    if (cls_out)
        *cls_out = byte;
    if (pa_out)
        *pa_out = arch_pte_addr(pte) + (addr & (PAGE_SIZE - 1));
    cursor_leaf_unlock(cur);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Deferred page-table page reclamation (P4)
 * ------------------------------------------------------------------ */
void mm_pt_read_enter(struct mm_struct *mm)
{
    if (mm)
        __atomic_fetch_add(&mm->pt_readers, 1, __ATOMIC_ACQ_REL);
}

void mm_pt_read_exit(struct mm_struct *mm)
{
    if (mm)
        __atomic_fetch_sub(&mm->pt_readers, 1, __ATOMIC_ACQ_REL);
}

int mm_pt_defer_free(struct mm_struct *mm, pte_t *table, int level)
{
    if (!mm || !table)
        return -EINVAL;
    pfn_t pfn = virt_to_pfn((const void *)table);
    if (!pfn_valid(pfn))
        return -EINVAL;
    int r = mm_pt_hold_table(mm, pfn, level);
    if (r < 0)
        return r;
    /* The page is now unreachable through the page table; a cursor that had
     * already cached it either still holds a read-side reference (so the
     * drain waits) or will observe `stale` and retry. */
    mm_pt_node_fini(table);
    return 0;
}

/*
 * Grace-period reclamation for detached page-table pages.
 *
 * mm_addrspace_lock() brackets the entire cursor lifetime with
 * mm_pt_read_enter()/mm_pt_read_exit() and reads each parent entry exactly
 * once on the way down.  So once the unlink from the parent has been
 * published, no *new* cursor can reach this page, and every cursor that
 * already holds it is counted in mm->pt_readers.  Marking the subtree stale
 * makes those cursors abandon the node (mm_addrspace_lock re-checks `stale`
 * under the lock and retries); returning the *frame* only after pt_readers
 * has been zero closes the remaining window.
 *
 * The frame deliberately keeps FRAME_F_PT while it waits, so the buddy cannot
 * hand it to anyone else; the metadata is dropped at drain time, which is
 * what makes a cached pointer read back as "no metadata" for an already
 * detached node.
 *
 * This is intentionally independent of the TLB transaction.  A PT page being
 * unreachable through the page table has nothing to do with which addresses a
 * shootdown transaction dirtied, and coupling the two would force every
 * teardown call site to open a transaction and hold mm->lock -- several of
 * them (vma.c free_vma_pages, sysv_shm, vma.c's demote call) do neither.
 */
void mm_pt_retire_drain(mm_struct_t *mm)
{
    if (!mm)
        return;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mm->pt_retire_lock);
        mm_pt_retire_t *list = mm->pt_retire;
        /* Only reclaim when no cursor is inside a read-side section.  A cursor
         * that arrives after this check cannot reach these pages: they are
         * already unlinked. */
        if (!list || __atomic_load_n(&mm->pt_readers, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&mm->pt_retire_lock, flags);
            return;
        }
        mm->pt_retire = NULL;
        spin_unlock_irqrestore(&mm->pt_retire_lock, flags);

        while (list) {
            mm_pt_retire_t *next = list->next;
            pfn_t pfn = list->frame;
            if (pfn_valid(pfn)) {
                pte_t *table = pfn_to_virt(pfn);
                mm_pt_node_fini(table);
                pfa_free(pfn, list->level == ARCH_PT_ROOT_LEVEL
                                    ? ARCH_PT_ROOT_ORDER : 0);
            }
            kfree(list);
            list = next;
        }
        /* Re-check: a retire that raced us is still ours to reclaim. */
    }
}

void mm_pt_retire_table(mm_struct_t *mm, pte_t *table, int level)
{
    if (!table)
        return;
    pfn_t pfn = virt_to_pfn((const void *)table);
    if (!pfn_valid(pfn))
        return;
    if (!mm) {
        mm_pt_node_fini(table);
        frame_free(table);
        return;
    }

    /* Any cursor that cached this node observes `stale` and re-descends. */
    mm_pt_mark_stale_recursive(table, level);

    mm_pt_retire_t *r = kcalloc_atomic(1, sizeof(*r));
    if (!r) {
        /* Cannot queue.  Fall back to freeing now, which is exactly the
         * behaviour (and the exposure) this path replaces -- a rare OOM path
         * is not worth leaking a page-table page over. */
        mm_pt_node_fini(table);
        frame_free(table);
        return;
    }
    r->frame = pfn;
    r->level = (uint8_t)level;

    uint64_t flags = spin_lock_irqsave(&mm->pt_retire_lock);
    r->next = mm->pt_retire;
    mm->pt_retire = r;
    spin_unlock_irqrestore(&mm->pt_retire_lock, flags);

    /* Reclaim straight away when nobody is traversing; otherwise the next
     * retire, or the teardown drain, will pick it up. */
    mm_pt_retire_drain(mm);
}

void mm_pt_mark_stale_recursive(pte_t *table, int level)
{
    pt_meta_t *m = mm_pt_meta(table);
    if (m)
        mm_pt_meta_set_stale(m, 1);
    if (level <= 0)
        return;
    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        pte_t e = table[i];
        if ((e & PTE_V) && !arch_pte_is_leaf(e))
            mm_pt_mark_stale_recursive(arch_pte_to_ptr(e), level - 1);
    }
}

/* ------------------------------------------------------------------ *
 * Auditor
 * ------------------------------------------------------------------ *
 * The auditor turns the migration into a machine-checked equivalence instead
 * of a leap of faith: it walks the hardware page tables and the per-PTE
 * metadata independently and reports every disagreement.  It is the only
 * sanctioned consumer of a raw page-table walk outside teardown.
 */

/* The status byte covering `addr`, at whichever level actually holds the
 * leaf.  mm_pt_leaf_table() stops at the leaf's own table, which for a
 * megapage VMA is a level above it -- and there is no level-0 metadata to
 * read, so asking only level 0 makes every THP-backed VMA look like the
 * status forgot about it.  That is a false positive in the auditor, not a
 * defect in the mutator, and a gate that cries wolf gets switched off.
 *
 * Returns MM_ST_INVALID both for a genuinely unknown address and for an
 * unmapped one; the callers here all ask "does the status claim this
 * address?", for which the two are the same answer. */
static uint8_t audit_status_at(pt_root_t *pgdir, vaddr_t addr)
{
    pte_t *table = pgdir;
    for (int l = ARCH_PT_ROOT_LEVEL; l > 0; l--) {
        pte_t e = table[arch_pt_vpn(addr, l)];
        if (!(e & PTE_V) || arch_pte_is_leaf(e))
            return MM_ST_GET_CLASS(mm_pt_peek(table, l, arch_pt_vpn(addr, l)));
        table = arch_pte_to_ptr(e);
    }
    return MM_ST_GET_CLASS(mm_pt_peek(table, 0, arch_pt_vpn(addr, 0)));
}

/* Does the hardware map a leaf at `addr`?  Counterpart to audit_status_at():
 * the pair (status, PTE) has to be read together to tell "the status forgot a
 * page that is really mapped" apart from "this address is a hole". */
static int audit_pte_present(pt_root_t *pgdir, vaddr_t addr)
{
    pte_t *table = pgdir;
    for (int l = ARCH_PT_ROOT_LEVEL; l > 0; l--) {
        pte_t e = table[arch_pt_vpn(addr, l)];
        if (!(e & PTE_V))
            return 0;
        if (arch_pte_is_leaf(e))
            return 1;
        table = arch_pte_to_ptr(e);
    }
    pte_t e = table[arch_pt_vpn(addr, 0)];
    return (e & PTE_V) && arch_pte_is_leaf(e);
}

/* Is `cls` a class the VMA could legitimately have produced?
 *
 * This is deliberately a compatibility predicate and not an equality test.
 * A MAP_PRIVATE file page that has been written to is class FILE_PRIVATE
 * before the COW break and ANON_MAPPED after it, and both are correct at
 * different times; the same holds for a VMO page and for a page that fork
 * made shared.  Demanding equality would fire on all of those.
 *
 * What it does rule out is the case where the two representations disagree
 * about something coherence depends on.  The load-bearing rule is MAP_SHARED
 * file: there the leaf is the canonical page-cache frame and is written in
 * place, never copied, so an anonymous class in that VMA means a write would
 * go to a private frame while the file's readers keep the cache frame.  The
 * symmetric rule matters just as much -- a FILE_* class inside a VMA with no
 * file behind it means a private frame is being taken for a cache page.
 */
static int audit_class_compatible(uint8_t cls, const mm_seg_t *v)
{
    /* A swapped-out page has no class to speak of yet; where its contents
     * live is a separate question, and P7 is the stage that answers it. */
    if (cls == MM_ST_SWAPPED)
        return 1;

    int file_shared = (v->vm_flags & VM_FILE) && (v->vm_flags & VM_SHARED);
    int file_private = (v->vm_flags & VM_FILE) && !(v->vm_flags & VM_SHARED);
    int vmo = (v->vm_flags & VM_VMO) != 0;

    if (file_shared)
        return cls == MM_ST_FILE_SHARED;
    if (file_private)
        return cls == MM_ST_FILE_PRIVATE || cls == MM_ST_ANON_MAPPED ||
               cls == MM_ST_ANON_SHARED;
    if (vmo)
        return cls == MM_ST_VMO || cls == MM_ST_ANON_MAPPED ||
               cls == MM_ST_ANON_SHARED;
    return cls == MM_ST_ANON_MAPPED || cls == MM_ST_ANON_SHARED;
}
static uint64_t audit_table(pte_t *table, int level, int is_root,
                            mm_pt_audit_report_t *rep, mm_struct_t *mm,
                            vaddr_t base, int check_vma)
{
    pt_meta_t *m = mm_pt_meta(table);
    rep->pt_pages++;
    if (!m) {
        rep->missing_meta++;
        return 0;
    }

    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        pte_t pte = table[i];
        vaddr_t va = base + (vaddr_t)i *
                               ((vaddr_t)PAGE_SIZE << (ARCH_PT_BITS * level));
        uint8_t byte = m->cls[i];
        uint8_t cls = MM_ST_GET_CLASS(byte);
        int cow = (byte & MM_ST_COW_BIT) ? 1 : 0;

        /* The shared kernel half is owned by the boot page table. */
        if (is_root && i >= ARCH_PT_USER_END)
            continue;

#ifdef CONFIG_SWAP
        if (pte_is_swap(pte)) {
            if (cls != MM_ST_SWAPPED)
                rep->absent_mismatch++;
            continue;
        }
#endif

        int present = (pte & PTE_V) && arch_pte_is_leaf(pte);
        int is_node = (pte & PTE_V) && !arch_pte_is_leaf(pte);

        if (is_node) {
            if (cls != MM_ST_PT_NODE)
                rep->present_mismatch++;
            /* A segment index lives on THIS entry -- the parent's slot for a
             * child node -- so it is checked here and not on the leaf.  A
             * non-zero index that resolves to nothing is the dangerous case:
             * fault dispatch would follow a freed vnode. */
            if (level >= 1 && m->segtab && m->segtab->idx[i]) {
                /* An entry names up to MM_SEGTAB_MAX segments, so every nibble
                 * is checked, not just the first. */
                for (int k = 0; k < MM_SEGTAB_NAMES; k++) {
                    uint8_t slot = segtab_slot_get(m->segtab, i, k);
                    if (!slot)
                        continue;
                    rep->seg_slots++;
                    mm_seg_t *s = segtab_seg(m->segtab, slot);
                    if (!s || s->magic != MM_SEG_MAGIC) {
                        if (!rep->seg_bad_slot)
                            rep->seg_bad_va = va;
                        rep->seg_bad_slot++;
                        continue;
                    }
                    rep->seg_pages++;
                    if (check_vma) {
                        mm_seg_t *v = mm_seg_find(mm, va);
                        uint8_t want = (v && (v->vm_flags & VM_VMO))
                                           ? MM_SEG_VMO
                                           : ((v && (v->vm_flags & VM_FILE))
                                                  ? MM_SEG_FILE : MM_SEG_ANON);
                        /* Only judged when the segment actually claims this
                         * address: outside its extent it is not authoritative
                         * and the reader falls back to the VMA anyway.
                         *
                         * The `s->kind != MM_SEG_ANON` exemption this used to
                         * carry is gone, and it was dead weight: no anonymous
                         * mapping had a segment, so MM_SEG_ANON only ever
                         * appeared here as the "VMA has no backing object"
                         * value of `want`.  Now that anonymous mappings do get
                         * MM_SEG_ANON segments, keeping the exemption would
                         * have exempted exactly the new segments from the one
                         * check that is supposed to catch a mislabelled
                         * mapping -- and a mapping that drifted from anon to
                         * file would have gone unreported. */
                        if (v && va >= s->start && va < s->end &&
                            mm_seg_kind(s) != want) {
                            if (!rep->seg_kind_mismatch) {
                                rep->seg_kind_bad_va = va;
                                rep->seg_kind_bad = mm_seg_kind(s);
                            }
                            rep->seg_kind_mismatch++;
                        }
                    }
                }
            }
            /* Accumulate this level's offset: the child table covers
             * [va, va + span) -- passing base unchanged would report the
             * child's entries at the wrong address. */
            audit_table(arch_pte_to_ptr(pte), level - 1, 0, rep, mm, va, check_vma);
            continue;
        }

        if (present) {
            if (cls == MM_ST_INVALID || cls == MM_ST_PT_NODE)
                rep->present_mismatch++;
            if ((byte & MM_ST_PROT_MASK) != mm_pt_prot_bits(pte))
                rep->prot_mismatch++;
            if (cow != ((pte & PTE_COW) ? 1 : 0))
                rep->cow_mismatch++;
        } else {
            /* Absent: the status must be Invalid, or one of the two classes
             * that legitimately exist without a PTE -- a virtually allocated
             * page awaiting on-demand paging, or a swapped-out page. */
            if (cls != MM_ST_INVALID && cls != MM_ST_ANON_VIRT &&
                cls != MM_ST_SWAPPED)
                rep->absent_mismatch++;
            else if (cls == MM_ST_ANON_VIRT)
                rep->anon_virt++;
            if (cow)
                rep->cow_mismatch++;
        }

        if (level == 0) {
            rep->entries++;
            /* MM_AS_MODEL reverse direction (P8).  The loop above asks, per
             * VMA, "does the status know about this range?".  This asks the
             * converse for every individual leaf: if the status claims
             * anything at all -- Mapped, COW, or reserved-but-unbacked
             * MM_AS_ANON_VIRT -- then some VMA must account for the address.
             *
             * Without it a page can be mapped with no VMA covering it and the
             * forward check still passes, because the forward check only ever
             * starts from a VMA.  That is precisely what "the VMA list is
             * purely derived" has to exclude, so the invariant is stated as
             * its own counter rather than folded into vma_mismatch: a reader
             * must be able to tell which direction broke.
             *
             * MM_ST_INVALID means the status claims nothing, which is exactly
             * the case where no VMA is required -- a hole is not an omission. */
            if (cls != MM_ST_INVALID && check_vma) {
                mm_seg_t *v = mm_seg_find(mm, va);
                if (!v || va < v->start || va >= v->end) {
                    if (!rep->vmai_mismatch)
                        rep->vmai_bad_va = va;
                    rep->vmai_mismatch++;
                } else if (present && !audit_class_compatible(cls, v)) {
                    /* The two representations agree that something is mapped
                     * here but not on WHAT it is.  This is the counter P6 is
                     * gated on: switching fault dispatch to the status is only
                     * sound while this is 0, because dispatch asks "anonymous
                     * or file?" and today it gets the right answer from the
                     * VMA while the status is free to be wrong about it.
                     *
                     * Restricted to present leaves on purpose: an absent page
                     * carrying the wrong class is a separate, known gap (an
                     * un-faulted file mapping has no class at all), and folding
                     * it in here would blur two different things. */
                    if (!rep->cls_mismatch) {
                        rep->cls_bad_va = va;
                        rep->cls_bad_class = cls;
                    }
                    rep->cls_mismatch++;
                }
            }
        }
    }
    return 0;
}

int mm_pt_audit_addrspace(mm_struct_t *mm, int check_vma,
                          mm_pt_audit_report_t *out)
{
    mm_pt_audit_report_t local;
    mm_pt_audit_report_t *rep = out ? out : &local;

    memset(rep, 0, sizeof(*rep));
    if (!mm || !mm->pgdir)
        return -EINVAL;

    audit_table(mm->pgdir, ARCH_PT_ROOT_LEVEL, 1, rep, mm, 0, check_vma);

    /* The mapping list, checked as the only representation there is.  When the
     * page tables and the list were two answers to the same question there was
     * an index shadow check here, comparing the two.  There is one answer now,
     * so what is worth asserting is the property every lookup now rests on:
     * the list is sorted by start, the entries do not overlap, and each one is
     * a live record.  mm_seg_find()'s binary search returns a wrong mapping if
     * any of those fails, and there is no second implementation to catch it. */
    if (check_vma) {
        mm_seg_t *prev = NULL;
        for (mm_seg_t *v = mm->mmap; v; v = v->next) {
            rep->seg_extent_vmas++;
            if (v->magic != MM_SEG_MAGIC || v->end <= v->start) {
                rep->seg_extent_noseg++;
            } else if (prev && v->start < prev->end) {
                rep->seg_extent_mismatch++;
            } else {
                rep->seg_pte_agree++;
            }
            prev = v;
        }
    }

    /* Mapping cross-check.  A mapping asserts an interval is mapped; the
     * metadata asserts per page.  Different granularities, so the consistency
     * rule is not derivable from either side.  A mutator that updates one
     * representation and forgets the other shows up here first. */
    if (check_vma) {
        for (mm_seg_t *v = mm->mmap; v; v = v->next) {
            /* The rule is "a VMA with resident pages must have the status
             * know about at least one of them", NOT "a VMA must have a page
             * the status knows about".
             *
             * The second, weaker-sounding form is what this check used to
             * assert, and it is wrong: a VMA is an *authorisation* to map,
             * while the status is *state*.  A freshly mmap'd region that the
             * process never touched has a VMA and no metadata for any of its
             * pages, which is completely legal -- on-demand paging is the
             * mechanism the whole model is built on.  Asserting otherwise
             * made the auditor fire on ordinary programs, which is how a gate
             * earns the right to be ignored.
             *
             * Requiring agreement only when the hardware already maps
             * something is the decidable version: an empty VMA is fine, and a
             * VMA holding present PTE leaves whose status is Invalid is
             * exactly the "mutator wrote one representation and forgot the
             * other" case this counter exists for. */
            int any_known = 0;
            int any_resident = 0;
            for (vaddr_t va = v->start & ~(vaddr_t)(PAGE_SIZE - 1);
                 va < v->end && !any_known; va += PAGE_SIZE) {
                if (audit_status_at(mm->pgdir, va) != MM_ST_INVALID)
                    any_known = 1;
                else if (!any_resident && audit_pte_present(mm->pgdir, va))
                    any_resident = 1;
            }
            if (any_resident && !any_known) {
                if (!rep->vma_mismatch)
                    rep->vma_bad_va = v->start;
                rep->vma_mismatch++;
            }
        }

        /* Safety-bit cross-check.  MM_SAFE_NO_FA must agree with VM_SEALED in
         * both directions for every *mapped* page: a sealed range that a
         * fault-around could still pull in defeats the seal, and the reverse
         * would needlessly suppress fault-around.  This is what makes the
         * mseal() wiring machine-checked rather than merely plausible.
         *
         * MM_SAFE_UFFD is deliberately NOT checked here.  UFFDIO_UNREGISTER
         * clears the mark for the union of the ranges it removed, so a page
         * still covered by a *different* registration loses its mark; the
         * authoritative test is userfaultfd_range_present() on the VMA fault
         * path.  Checking it would report mismatches by construction until
         * that path clears per page after re-testing presence (docs 10.19). */
        for (mm_seg_t *v = mm->mmap; v; v = v->next) {
            int want = (v->vm_flags & VM_SEALED) ? 1 : 0;
            for (vaddr_t va = v->start & ~(vaddr_t)(PAGE_SIZE - 1);
                 va < v->end; va += PAGE_SIZE) {
                pte_t *t = mm_pt_leaf_table(mm->pgdir, va);
                if (!t)
                    continue;
                int idx = arch_pt_vpn(va, 0);
                /* Unmapped entries legitimately carry no safety bit. */
                if (MM_ST_GET_CLASS(mm_pt_peek(t, 0, idx)) == MM_ST_INVALID)
                    continue;
                if (mm_pt_safe_test(t, 0, idx, MM_SAFE_NO_FA) != want)
                    rep->safe_mismatch++;
            }
        }
    }

    return mm_pt_audit_errors(rep) ? -EFAULT : 0;
}

/*
 * Audit every live address space.  Run at shutdown next to the existing
 * pfa_audit_lists() buddy check, so every boot of every smoke test proves the
 * two representations agreed across the whole workload rather than at one
 * sampled instant.
 */
int mm_pt_audit_all(mm_pt_audit_report_t *out)
{
    mm_pt_audit_report_t local;
    mm_pt_audit_report_t *rep = out ? out : &local;
    memset(rep, 0, sizeof(*rep));

    uint64_t pf = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED || !t->mm)
            continue;
        mm_struct_t *mm = t->mm;
        spin_lock(&mm->lock);
        mm_pt_audit_addrspace(mm, 1, rep);
        spin_unlock(&mm->lock);
    }
    spin_unlock_irqrestore(&proc_lock, pf);

    return mm_pt_audit_errors(rep) ? -EFAULT : 0;
}

#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */
