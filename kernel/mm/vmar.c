/*
 * Core VMAR object engine (docs/zh/native-abi/04-memory.md §3).
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
#include "mm/vm.h"

/*
 * The reservation engine lives in kernel/mm/vmar_policy.rs.  What is left here
 * is the node's storage and the primitives the decisions need: kmalloc/kfree,
 * the spinlock, the refcount, and the list surgery (link, unlink and the tail
 * recompute).  None of these decides anything.
 */
extern vmar_t *vmar_create_root(struct mm_struct *mm, uint64_t base,
                                uint64_t len, uint32_t cap);
extern int64_t vmar_create_child(vmar_t *parent, uint64_t base, uint64_t len,
                                 uint32_t cap_request, vmar_t **out);
extern int vmar_contains(const vmar_t *v, uint64_t addr, uint64_t len);
extern int vmar_cap_allows(const vmar_t *v, uint32_t map_bits);
extern void vmar_acquire(vmar_t *v);
extern void vmar_release(vmar_t *v);

struct a20_vmar_geometry {
    uint64_t  base;
    uint64_t  len;
    uint32_t  cap;
    uint32_t  pad;
    vmar_t   *parent;
};
_Static_assert(sizeof(struct a20_vmar_geometry) == 32, "Rust/C vmar geometry ABI");

uint64_t a20_vmar_user_va_limit(void)
{
    return USER_VA_LIMIT;
}

vmar_t *a20_vmar_alloc(uint64_t base, uint64_t len, uint32_t cap)
{
    vmar_t *v = kmalloc(sizeof(*v));
    if (!v)
        return NULL;
    memset(v, 0, sizeof(*v));
    v->base = base;
    v->len = len;
    v->cap = cap;
    spin_init(&v->lock);
    refcount_set(&v->refs, 1); /* caller's reference */
    return v;
}

void a20_vmar_set_parent(vmar_t *v, vmar_t *parent)
{
    v->parent = parent;
}

void a20_vmar_geometry(const vmar_t *v, struct a20_vmar_geometry *out)
{
    out->base = v->base;
    out->len = v->len;
    out->cap = v->cap;
    out->pad = 0;
    out->parent = v->parent;
}

/* The first child starting at or above `base`, with the last child starting
 * below it reported through `out_pred`.  The ordered, non-overlapping list is
 * what makes those the only two candidates; the overlap test is the caller's. */
vmar_t *a20_vmar_locate(vmar_t *parent, uint64_t base, vmar_t **out_pred)
{
    vmar_t *pred = NULL;
    vmar_t *cur = parent->child_head;
    while (cur && cur->base < base) {
        pred = cur;
        cur = cur->sibling_next;
    }
    *out_pred = pred;
    return cur;
}

void a20_vmar_link(vmar_t *parent, vmar_t *child, vmar_t *pred)
{
    if (pred) {
        child->sibling_next = pred->sibling_next;
        pred->sibling_next = child;
    } else {
        child->sibling_next = parent->child_head;
        parent->child_head = child;
    }
    if (!child->sibling_next)
        parent->child_tail = child;
    parent->child_count++;
}

/* Unlink and fix up the tail fast path.  child_tail is only a shortcut for
 * appends, so it is recomputed from the surviving head when the unlinked node
 * was the tail -- once, on the last child of a dying parent, not per insert. */
void a20_vmar_unlink(vmar_t *parent, vmar_t *v)
{
    uint64_t flags = spin_lock_irqsave(&parent->lock);
    vmar_t **pp = &parent->child_head;
    while (*pp && *pp != v)
        pp = &(*pp)->sibling_next;
    if (*pp) {
        *pp = v->sibling_next;
        parent->child_count--;
    }
    if (parent->child_tail == v) {
        vmar_t *last = parent->child_head;
        while (last && last->sibling_next)
            last = last->sibling_next;
        parent->child_tail = last;
    }
    spin_unlock_irqrestore(&parent->lock, flags);
}

void a20_vmar_free(vmar_t *v)
{
    kfree(v);
}

uint64_t a20_vmar_lock(vmar_t *parent)
{
    return spin_lock_irqsave(&parent->lock);
}

void a20_vmar_unlock(vmar_t *parent, uint64_t flags)
{
    spin_unlock_irqrestore(&parent->lock, flags);
}

void a20_vmar_get(vmar_t *v)
{
    refcount_inc(&v->refs);
}

int a20_vmar_put(vmar_t *v)
{
    return refcount_dec_and_test(&v->refs) ? 1 : 0;
}

