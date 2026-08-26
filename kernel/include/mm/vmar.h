/*
 * Hierarchical VMAR (Virtual Memory Address Region) — core object model.
 * Design: docs/native-abi/04-memory.md §3 (Zircon-style sub-allocation).
 *
 * A VMAR is a reserved address-range node.  Children sub-allocate from
 * parents; capability ceilings only narrow down the tree; mappings created
 * through a VMAR record their owner so later protect/unmap decisions can
 * consult the ceiling that authorized them.
 */
#ifndef _MM_VMAR_H
#define _MM_VMAR_H

#include "core/types.h"
#include "core/refcount.h"

struct mm_struct;
struct vmar;

/* Canonical mapping-ceiling bits.  The Native ABI re-exports these as
 * A20_VMAR_CAN_MAP_* (see kernel/include/abi/native/types.h). */
#define VMAR_CAN_MAP_READ       0x01ull
#define VMAR_CAN_MAP_WRITE      0x02ull
#define VMAR_CAN_MAP_EXEC       0x04ull
#define VMAR_CAN_MAP_SPECIFIC   0x08ull  /* fixed-address maps allowed */

typedef struct vmar {
    uint64_t        base;
    uint64_t        len;
    uint32_t        cap;            /* never exceeds parent->cap */
    uint32_t        child_count;
    struct vmar    *parent;
    struct vmar    *sibling_next;   /* chain under one parent */
    struct vmar    *child_head;
    refcount_t      refs;           /* handle refs + child refs + map refs */
} vmar_t;

/* Root VMAR over [base, base+len).  NULL on bad range/OOM. */
vmar_t *vmar_create_root(struct mm_struct *mm, uint64_t base, uint64_t len,
                         uint32_t cap);

/* Sub-allocate [base, base+len) under parent.  Rights only narrow:
 * effective cap = cap_request & parent->cap.  Fails on unaligned or
 * out-of-parent ranges and on overlap with an existing sibling. */
int64_t vmar_create_child(vmar_t *parent, uint64_t base, uint64_t len,
                          uint32_t cap_request, vmar_t **out);

/* Containment + ceiling query for map/protect routing.  map_bits are
 * VMAR_CAN_MAP_*; the caller translates its own protection encoding. */
int     vmar_contains(const vmar_t *v, uint64_t addr, uint64_t len);
int     vmar_cap_allows(const vmar_t *v, uint32_t map_bits);

void    vmar_acquire(vmar_t *v);
/* Drop one reference; frees the node at zero (children keep parents alive). */
void    vmar_release(vmar_t *v);

#endif /* _MM_VMAR_H */
