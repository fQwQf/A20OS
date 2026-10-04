/*
 * Core VMAR object engine (docs/native-abi/04-memory.md §3).
 *
 * The tree is a reservation hierarchy only: page tables and VMAs stay in
 * the owning mm_struct.  A VMAR adds three rules on top of plain mmap:
 *   1. every child range lies inside its parent and never overlaps a
 *      sibling,
 *   2. capability ceilings narrow monotonically down the tree,
 *   3. mappings routed through a VMAR remember the node, so protect
 *      checks can re-apply the ceiling that authorized them.
 *
 * Concurrency: a parent VMAR is reachable from handles held by several tasks,
 * so its child list is guarded by the node's own spinlock.  The no-overlap
 * rule only means anything if the test and the insert are one atomic step --
 * otherwise two tasks can both observe a free range and both insert.  That
 * lock is a leaf: nothing else is acquired while holding it, and it is never
 * held across a sleep, an allocation, or a call into the VMA/VMO layers.
 */
#include "core/types.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/slab.h"
#include "mm/swap.h"      /* PAGE_SIZE */
#include "mm/vmar.h"

vmar_t *vmar_create_root(struct mm_struct *mm, uint64_t base, uint64_t len,
                         uint32_t cap)
{
    (void)mm;
    if (len == 0 || base + len < base) return NULL;
    if ((base & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1))) return NULL;

    vmar_t *v = kmalloc(sizeof(*v));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->base = base;
    v->len = len;
    v->cap = cap & (VMAR_CAN_MAP_READ | VMAR_CAN_MAP_WRITE |
                    VMAR_CAN_MAP_EXEC | VMAR_CAN_MAP_SPECIFIC);
    spin_init(&v->lock);
    refcount_set(&v->refs, 1); /* caller's reference */
    return v;
}

static int ranges_overlap(uint64_t a1, uint64_t l1, uint64_t a2, uint64_t l2)
{
    return a1 < a2 + l2 && a2 < a1 + l1;
}

/*
 * Locate [base, base+len) among the children of `parent`, which are ordered by
 * base.  *pred_out is the last child starting strictly below `base`, and the
 * return value is the first child starting at or above it -- which is both the
 * first possible overlap and the node the new child must be linked after.
 *
 * Only those two can overlap: anything starting below *pred ends before
 * *pred->base, since the list is non-overlapping by construction.
 */
static vmar_t *vmar_locate_sibling(vmar_t *parent, uint64_t base, uint64_t len,
                                   vmar_t **pred_out)
{
    vmar_t *pred = NULL;
    vmar_t *cur = parent->child_head;
    while (cur && cur->base < base) {
        pred = cur;
        cur = cur->sibling_next;
    }
    *pred_out = pred;
    if (pred && ranges_overlap(base, len, pred->base, pred->len))
        return pred;
    if (cur && ranges_overlap(base, len, cur->base, cur->len))
        return cur;
    return NULL;
}

int64_t vmar_create_child(vmar_t *parent, uint64_t base, uint64_t len,
                          uint32_t cap_request, vmar_t **out)
{
    if (!parent || !out) return -EINVAL;
    if (len == 0 || base + len < base) return -EINVAL;
    if ((base & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)))
        return -EINVAL;
    if (!vmar_contains(parent, base, len))
        return -ENOSPC;

    /* Allocated outside the lock: kmalloc can reclaim, and no lock may be held
     * across that.  The node stays unpublished until the insert below. */
    vmar_t *v = vmar_create_root(NULL, base, len, cap_request & parent->cap);
    if (!v) return -ENOMEM;
    v->parent = parent;

    uint64_t flags = spin_lock_irqsave(&parent->lock);
    vmar_t *pred = NULL;
    if (vmar_locate_sibling(parent, base, len, &pred)) {
        spin_unlock_irqrestore(&parent->lock, flags);
        kfree(v);
        return -ENOSPC;
    }

    if (pred) {
        v->sibling_next = pred->sibling_next;
        pred->sibling_next = v;
    } else {
        v->sibling_next = parent->child_head;
        parent->child_head = v;
    }
    if (!v->sibling_next)
        parent->child_tail = v;
    parent->child_count++;
    vmar_acquire(parent); /* child keeps parent alive */
    spin_unlock_irqrestore(&parent->lock, flags);

    *out = v;
    return 0;
}

int vmar_contains(const vmar_t *v, uint64_t addr, uint64_t len)
{
    if (!v || len == 0 || addr + len < addr) return 0;
    return addr >= v->base && len <= v->len && addr - v->base <= v->len - len;
}

int vmar_cap_allows(const vmar_t *v, uint32_t map_bits)
{
    return (map_bits & ~(v->cap & (VMAR_CAN_MAP_READ |
                                   VMAR_CAN_MAP_WRITE |
                                   VMAR_CAN_MAP_EXEC))) == 0;
}

void vmar_acquire(vmar_t *v)
{
    if (v) refcount_inc(&v->refs);
}

void vmar_release(vmar_t *v)
{
    if (!v) return;
    if (refcount_dec_and_test(&v->refs)) {
        vmar_t *parent = v->parent;
        if (parent) {
            /* Hold the parent across the unlink: this may be the last
             * reference to it, and it must not be freed underneath the lock
             * this node is about to be removed under. */
            vmar_acquire(parent);

            uint64_t flags = spin_lock_irqsave(&parent->lock);
            vmar_t **pp = &parent->child_head;
            while (*pp && *pp != v)
                pp = &(*pp)->sibling_next;
            if (*pp) {
                *pp = v->sibling_next;
                parent->child_count--;
            }
            /* child_tail is only a fast path for appends; recompute it from the
             * surviving head when v was the tail.  This walk happens once, on
             * the last child of a dying parent, not on every insert. */
            if (parent->child_tail == v) {
                vmar_t *last = parent->child_head;
                while (last && last->sibling_next)
                    last = last->sibling_next;
                parent->child_tail = last;
            }
            spin_unlock_irqrestore(&parent->lock, flags);

            /* Recurse only once the list is consistent and the lock is dropped,
             * so a cascading teardown cannot re-enter this same list. */
            vmar_release(parent);
        }
        kfree(v);
    }
}
