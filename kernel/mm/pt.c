#include "core/defs.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/cpu.h"
#include "core/perf.h"
#include "core/string.h"
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
    m->node = (uintptr_t)me;

    uintptr_t tail = __atomic_exchange_n(&m->lock, (uintptr_t)me,
                                         __ATOMIC_ACQ_REL);
    a20_perf_count(A20_PERF_MM_PT_LOCK_ACQUIRES);
    if (tail) {
        a20_perf_count(A20_PERF_MM_PT_LOCK_CONTENDED);
        a20_perf_count(A20_PERF_MM_PT_LOCK_WAITS);
        __atomic_store_n(&me->locked, 0, __ATOMIC_RELEASE);
        uint32_t spins = 0;
        while (__atomic_load_n(&me->locked, __ATOMIC_ACQUIRE) == 0) {
            arch_cpu_relax();
            /* Bounded spin: a page-table lock must never be held across a
             * blocking operation, so a long wait is a lock-order bug, not
             * contention.  Fail loudly with the waiter's own hold count --
             * which is the number of page-table nodes this CPU already has
             * locked, which is exactly the diagnostic that matters. */
            if (++spins == (1u << 26)) {
                kerr("[MCS DEADLOCK] cpu=%u waits on level=%u "
                     "already_holding=%u\\n", cpu, m->level,
                     (unsigned)(d - 1));
                panic("page-table lock wait exceeded bound");
            }
        }
    }
}

static void mcs_unlock(pt_meta_t *m)
{
    pt_mcs_node_t *me = (pt_mcs_node_t *)m->node;

    m->node = 0;
    pt_mcs_node_t *next =
        (pt_mcs_node_t *)__atomic_load_n(&me->next, __ATOMIC_ACQUIRE);
    if (next) {
        __atomic_store_n(&next->locked, 1, __ATOMIC_RELEASE);
    } else {
        /* We are the tail.  A compare-exchange rather than a store, so a
         * concurrent enqueue that already read the tail cannot be lost. */
        uintptr_t self = (uintptr_t)me;
        __atomic_compare_exchange_n(&m->lock, &self, 0, 0,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    /* Every acquisition pushed exactly one slot in mcs_lock, so every release
     * pops exactly one here.  This is the ONLY place the depth is
     * decremented: the descent loop's inline lock/unlock pairs and the
     * cursor's unwind loop both rely on it, and pre-decrementing in a caller
     * as well is a double decrement that corrupts the stack discipline. */
    g_pt_mcs_pool[pt_cpu()].depth--;
}

/* ------------------------------------------------------------------ *
 * Descriptor lifecycle
 * ------------------------------------------------------------------ */
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

    if (m)
        pfa_free(virt_to_pfn(m), pt_meta_order(sizeof(pt_meta_t)));
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

static inline uint8_t *cow_bit(pt_meta_t *m, int idx)
{
    if (!m || idx < 0 || idx >= MM_PT_META_ENTRIES)
        return NULL;
    return &m->cow[idx >> 3];
}

void mm_pt_note_present(pte_t *table, int level, int idx, uint8_t cls_byte)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    uint8_t *slot = cls_slot(m, idx);
    if (!slot)
        return;
    if (MM_ST_GET_CLASS(*slot) == MM_ST_INVALID)
        m->nr_present++;
    *slot = cls_byte;
    if (!(cls_byte & MM_ST_COW_BIT)) {
        uint8_t *cb = cow_bit(m, idx);
        if (cb)
            *cb &= (uint8_t)~(1u << (idx & 7));
    }
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
    uint8_t *cb = cow_bit(m, idx);
    if (cb)
        *cb &= (uint8_t)~(1u << (idx & 7));
}

uint8_t mm_pt_peek(pte_t *table, int level, int idx)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    uint8_t *slot = cls_slot(m, idx);
    return slot ? *slot : 0;
}

int mm_pt_cow(pte_t *table, int level, int idx)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    uint8_t *cb = cow_bit(m, idx);
    return (cb && (*cb & (1u << (idx & 7)))) ? 1 : 0;
}

void mm_pt_set_cow(pte_t *table, int level, int idx, int on)
{
    (void)level;
    pt_meta_t *m = mm_pt_meta(table);
    uint8_t *cb = cow_bit(m, idx);
    uint8_t *slot = cls_slot(m, idx);
    if (!cb || !slot)
        return;
    if (on) {
        *cb |= (uint8_t)(1u << (idx & 7));
        *slot |= MM_ST_COW_BIT;
    } else {
        *cb &= (uint8_t)~(1u << (idx & 7));
        *slot &= (uint8_t)~MM_ST_COW_BIT;
    }
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
    memcpy(dst->cow, src->cow, sizeof(src->cow));
    dst->nr_present = src->nr_present;
    dst->level = (uint8_t)level;
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

/* Index the cached path down to the leaf slot for addr, allocating any
 * missing intermediate node.  Caller holds the cursor, so the cached path
 * cannot be unlinked underneath it. */
static pte_t *cursor_leaf_slot(mm_cursor_t *cur, vaddr_t addr, int create)
{
    for (int l = cur->guard_level; l > 0; l--) {
        pte_t *table = cur->path[l];
        int idx = arch_pt_vpn(addr, l);
        pte_t e = table[idx];
        if (!(e & PTE_V)) {
            if (!create)
                return NULL;
            pte_t *next = (pte_t *)frame_alloc();
            if (!next)
                return NULL;
            mm_pt_node_init(next, l - 1);
            table[idx] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
            mm_pt_note_present(table, l, idx,
                               MM_ST_CLS_BYTE(MM_ST_PT_NODE));
            cur->path[l - 1] = next;
            continue;
        }
        if (arch_pte_is_leaf(e))
            return NULL;      /* huge leaf covers more than one page */
        cur->path[l - 1] = arch_pte_to_ptr(e);
    }
    return &cur->path[0][arch_pt_vpn(addr, 0)];
}

static inline pte_t *cursor_leaf_table(const mm_cursor_t *cur)
{
    return cur->path[0];
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

            pt_meta_t *pm = mm_pt_meta(table);
            if (!pm && mm_pt_node_init(table, l) < 0) {
                mm_pt_read_exit(mm);
                cur->in_read_side = 0;
                return -ENOMEM;
            }
            pm = mm_pt_meta(table);
            if (!pm) {
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
                mcs_unlock(pm);
                table = arch_pte_to_ptr(e);
                cur->path[l - 1] = table;
                continue;
            }
            if ((e & PTE_V) && arch_pte_is_leaf(e)) {
                mcs_unlock(pm);
                mm_pt_read_exit(mm);
                cur->in_read_side = 0;
                return 1;
            }

            pte_t *next = (pte_t *)frame_alloc();
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

    /* P3: the covering node's lock IS the unit of writer exclusion.  A
     * cursor only ever mutates the cached path below the covering node, and
     * every other cursor that could touch that path must first acquire this
     * same node, so it is sufficient and it is the finest granularity that
     * keeps disjoint ranges parallel.  Locking the whole subtree (the paper's
     * ADV step) belongs with the lockless-traverse + RCU design in P4; done
     * here it would both serialise unrelated ranges and risk double-locking
     * an aliased node. */
    cur->mm = mm;
    cur->locked = 1;
    a20_perf_count(A20_PERF_MM_CURSOR_OPEN);
    return 0;
}

void mm_cursor_unlock(mm_cursor_t *cur)
{
    if (!cur || !cur->locked || !cur->mm)
        return;
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

    pte_t *table = cursor_leaf_table(cur);
    int idx = arch_pt_vpn(addr, 0);
    pte_t *pte = &table[idx];

#ifdef CONFIG_SWAP
    if (pte_is_swap(*pte)) {
        swap_entry_t entry = pte_to_swp_entry(*pte);
        *pte = 0;
        mm_pt_note_absent(table, 0, idx);
        swap_free(entry);
        return 1;
    }
#endif
    if (!(*pte & PTE_V) || !arch_pte_is_leaf(*pte))
        return 0;

    pte_t old = *pte;
    *pte = 0;
    mm_pt_note_absent(table, 0, idx);
    frame_put(phys_to_pfn(arch_pte_addr(old)));
    return 1;
}

int mm_cursor_mark(mm_cursor_t *cur, vaddr_t addr, uint8_t cls)
{
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;
    if (cls >= MM_ST_CLASS_MAX)
        return -EINVAL;

    pte_t *table = cursor_leaf_table(cur);
    int idx = arch_pt_vpn(addr, 0);
    if (table[idx] & PTE_V)
        return -EEXIST;
    mm_pt_note_present(table, 0, idx, MM_ST_CLS_BYTE(cls));
    return 0;
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

    pte_t *table = cursor_leaf_table(cur);
    int idx = arch_pt_vpn(addr, 0);
    pte_t pte = table[idx];

#ifdef CONFIG_SWAP
    if (pte_is_swap(pte)) {
        if (cls_out)
            *cls_out = MM_ST_CLS_BYTE(MM_ST_SWAPPED) |
                       mm_pt_prot_bits(arch_pte_flags(pte));
        return 1;
    }
#endif
    if (!(pte & PTE_V) || !arch_pte_is_leaf(pte))
        return 0;

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
static uint64_t audit_table(pte_t *table, int level, int is_root,
                            mm_pt_audit_report_t *rep)
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
        uint8_t byte = m->cls[i];
        uint8_t cls = MM_ST_GET_CLASS(byte);
        int cow = (m->cow[i >> 3] & (1u << (i & 7))) ? 1 : 0;

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
            audit_table(arch_pte_to_ptr(pte), level - 1, 0, rep);
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
            if (cow)
                rep->cow_mismatch++;
        }

        if (level == 0)
            rep->entries++;
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

    audit_table(mm->pgdir, ARCH_PT_ROOT_LEVEL, 1, rep);

    /* VMA cross-check.  A VMA asserts an interval is mapped; the metadata
     * asserts per page.  Different granularities, so the consistency rule is
     * not derivable from either side: every VMA must have at least one page
     * the metadata knows about.  A mutator that updates one representation
     * and forgets the other shows up here first. */
    if (check_vma) {
        for (vm_area_t *v = mm->mmap; v; v = v->next) {
            int any = 0;
            for (vaddr_t va = v->start & ~(vaddr_t)(PAGE_SIZE - 1);
                 va < v->end && !any; va += PAGE_SIZE) {
                pte_t *t = mm_pt_leaf_table(mm->pgdir, va);
                if (t && MM_ST_GET_CLASS(mm_pt_peek(t, 0,
                                                    arch_pt_vpn(va, 0)))
                            != MM_ST_INVALID)
                    any = 1;
            }
            if (!any)
                rep->vma_mismatch++;
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
