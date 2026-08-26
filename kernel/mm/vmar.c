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
 */
#include "core/types.h"
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
    refcount_set(&v->refs, 1); /* caller's reference */
    return v;
}

static int ranges_overlap(uint64_t a1, uint64_t l1, uint64_t a2, uint64_t l2)
{
    return a1 < a2 + l2 && a2 < a1 + l1;
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

    for (vmar_t *sib = parent->child_head; sib; sib = sib->sibling_next)
        if (ranges_overlap(base, len, sib->base, sib->len))
            return -ENOSPC;

    vmar_t *v = vmar_create_root(NULL, base, len,
                                 cap_request & parent->cap);
    if (!v) return -ENOMEM;

    v->parent = parent;
    v->sibling_next = parent->child_head;
    parent->child_head = v;
    parent->child_count++;
    vmar_acquire(parent); /* child keeps parent alive */

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
            vmar_t **pp = &parent->child_head;
            while (*pp && *pp != v) pp = &(*pp)->sibling_next;
            if (*pp) {
                *pp = v->sibling_next;
                parent->child_count--;
                vmar_release(parent);
            }
        }
        kfree(v);
    }
}
