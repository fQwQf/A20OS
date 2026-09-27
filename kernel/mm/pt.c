#include "core/defs.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/cpu.h"
#include "core/string.h"
#include "mm/frame.h"
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
 * A cursor holds a path of page-table nodes, so one CPU may hold several
 * node locks at once.  Nodes are therefore per (cpu, depth) rather than per
 * cpu; ARCH_PT_LEVELS bounds the path length, so the pool is exactly large
 * enough and no allocation happens on the lock path.
 */
typedef struct pt_mcs_node {
    volatile uintptr_t next;
    volatile uintptr_t locked;
} pt_mcs_node_t;

static pt_mcs_node_t g_pt_mcs_pool[CONFIG_NR_CPUS][ARCH_PT_LEVELS];
static uint32_t       g_pt_mcs_depth[CONFIG_NR_CPUS];

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
    pt_mcs_node_t *me = &g_pt_mcs_pool[cpu][g_pt_mcs_depth[cpu]++];

    me->next = 0;
    me->locked = 1;
    m->node = (uintptr_t)me;

    uintptr_t tail = __atomic_exchange_n(&m->lock, (uintptr_t)me,
                                         __ATOMIC_ACQ_REL);
    if (tail) {
        __atomic_store_n(&me->locked, 0, __ATOMIC_RELEASE);
        while (__atomic_load_n(&me->locked, __ATOMIC_ACQUIRE) == 0)
            arch_cpu_relax();
    }
}

static void mcs_unlock(pt_meta_t *m)
{
    unsigned cpu = pt_cpu();
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
    g_pt_mcs_depth[cpu]--;
}

/* ------------------------------------------------------------------ *
 * Descriptor lifecycle
 * ------------------------------------------------------------------ */
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
 * paper's "covering PT page". */
static int pt_covering_level(vaddr_t start, vaddr_t end)
{
    int level = ARCH_PT_ROOT_LEVEL;
    while (level > 0) {
        vaddr_t span = (vaddr_t)PAGE_SIZE << (ARCH_PT_BITS * level);
        vaddr_t base = start & ~(span - 1);
        if (base + span >= end)
            break;
        level--;
    }
    return level;
}

/* Table that holds the leaf slot for addr, i.e. the parent of the leaf.  This
 * is the table whose metadata array describes the page. */
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

static pte_t *leaf_slot(pt_root_t *pgdir, vaddr_t addr, int *level_out)
{
    pte_t *table = pgdir;
    for (int l = ARCH_PT_ROOT_LEVEL; l > 0; l--) {
        pte_t e = table[arch_pt_vpn(addr, l)];
        if (!(e & PTE_V) || arch_pte_is_leaf(e)) {
            if (level_out)
                *level_out = l;
            return &table[arch_pt_vpn(addr, 0)];
        }
        table = arch_pte_to_ptr(e);
    }
    if (level_out)
        *level_out = 0;
    return &table[arch_pt_vpn(addr, 0)];
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
    pte_t *table = mm->pgdir;
    for (int l = ARCH_PT_ROOT_LEVEL; l > level; l--) {
        pte_t pte = table[arch_pt_vpn(start, l)];
        if (!(pte & PTE_V) || arch_pte_is_leaf(pte)) {
            /* A larger-than-needed leaf already covers the range.  The caller
             * must demote it (mm_demote_huge_page) before a transaction can
             * address individual pages inside it. */
            cur->mm = NULL;
            cur->start = start;
            cur->end = end;
            cur->locked = 0;
            return 1;
        }
        table = arch_pte_to_ptr(pte);
    }

    pt_meta_t *m = mm_pt_meta(table);
    if (!m && mm_pt_node_init(table, level) < 0)
        return -ENOMEM;
    m = mm_pt_meta(table);
    if (!m)
        return -ENOMEM;

    mcs_lock(m);
    if (m->stale) {
        mcs_unlock(m);
        return -EAGAIN;      /* racing a subtree detach; caller retries */
    }

    cur->mm = mm;
    cur->start = start;
    cur->end = end;
    cur->locked = 1;
    return 0;
}

void mm_cursor_unlock(mm_cursor_t *cur)
{
    if (!cur || !cur->locked || !cur->mm)
        return;
    int level = pt_covering_level(cur->start, cur->end);
    pte_t *table = cur->mm->pgdir;
    for (int l = ARCH_PT_ROOT_LEVEL; l > level; l--) {
        pte_t pte = table[arch_pt_vpn(cur->start, l)];
        if (!(pte & PTE_V) || arch_pte_is_leaf(pte))
            break;
        table = arch_pte_to_ptr(pte);
    }
    pt_meta_t *m = mm_pt_meta(table);
    if (m)
        mcs_unlock(m);
    cur->locked = 0;
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

int mm_cursor_query(mm_cursor_t *cur, vaddr_t addr, uint8_t *cls_out,
                    paddr_t *pa_out)
{
    if (cls_out)
        *cls_out = MM_ST_CLS_BYTE(MM_ST_INVALID);
    if (pa_out)
        *pa_out = 0;
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;

    pte_t *pte = leaf_slot(cur->mm->pgdir, addr, NULL);
    if (!pte)
        return 0;

#ifdef CONFIG_SWAP
    if (pte_is_swap(*pte)) {
        if (cls_out)
            *cls_out = MM_ST_CLS_BYTE(MM_ST_SWAPPED) |
                       mm_pt_prot_bits(arch_pte_flags(*pte));
        return 1;
    }
#endif
    if (!(*pte & PTE_V) || !arch_pte_is_leaf(*pte))
        return 0;

    /* The metadata is authoritative for the class; the PTE is authoritative
     * for the frame and for the effective permission bits. */
    pte_t *table = mm_pt_leaf_table(cur->mm->pgdir, addr);
    int idx = arch_pt_vpn(addr, 0);
    uint8_t byte = mm_pt_peek(table, 0, idx);
    if (MM_ST_GET_CLASS(byte) == MM_ST_INVALID)
        byte = MM_ST_CLS_BYTE(MM_ST_ANON_MAPPED);
    byte = (uint8_t)((byte & (uint8_t)~MM_ST_PROT_MASK) |
                     mm_pt_prot_bits(*pte));

    if (cls_out)
        *cls_out = byte;
    if (pa_out)
        *pa_out = arch_pte_addr(*pte) + (addr & (PAGE_SIZE - 1));
    return 1;
}

int mm_cursor_map(mm_cursor_t *cur, vaddr_t addr, paddr_t pa, pte_t flags,
                  uint8_t cls)
{
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;
    if (cls >= MM_ST_CLASS_MAX)
        return -EINVAL;

    pte_t *table = mm_pt_leaf_table(cur->mm->pgdir, addr);
    int idx = arch_pt_vpn(addr, 0);
    pte_t *pte = &table[idx];

    if (*pte & PTE_V) {
        paddr_t old_pa = arch_pte_addr(*pte);
        if (old_pa != pa && arch_pte_is_leaf(*pte))
            frame_put(phys_to_pfn(old_pa));
    }
    /* Executable mappings may be populated through PAGE_OFFSET before being
     * installed at their user VA; synchronise the I-cache before the PTE
     * becomes visible so demand paging, fork/COW, VMO maps and ELF loading
     * all obey the same contract. */
    if (flags & PTE_X) {
        pfn_t pfn = phys_to_pfn(pa);
        if (pfn_valid(pfn))
            arch_flush_icache_range(pfn_to_virt(pfn), PAGE_SIZE);
    }
    *pte = arch_pte_leaf(pa, flags);

    mm_pt_note_present(table, 0, idx,
                       (uint8_t)(MM_ST_CLS_BYTE(cls) |
                                 (flags & PTE_COW ? MM_ST_COW_BIT : 0) |
                                 mm_pt_prot_bits(flags)));
    return 0;
}

int mm_cursor_unmap(mm_cursor_t *cur, vaddr_t addr)
{
    if (!cursor_span_ok(cur, addr))
        return -EINVAL;

    pte_t *table = mm_pt_leaf_table(cur->mm->pgdir, addr);
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

    pte_t *table = mm_pt_leaf_table(cur->mm->pgdir, addr);
    int idx = arch_pt_vpn(addr, 0);
    if (table[idx] & PTE_V)
        return -EEXIST;
    mm_pt_note_present(table, 0, idx, MM_ST_CLS_BYTE(cls));
    return 0;
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
