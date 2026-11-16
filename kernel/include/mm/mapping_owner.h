#ifndef A20_MM_MAPPING_OWNER_H
#define A20_MM_MAPPING_OWNER_H

#include "core/refcount.h"

/* Storage/resource lifetime only: no mapping authority or DMA-stop proof.
 * The creator owns one reference. Borrowed arguments need a live reference;
 * copying a VMA takes another. Final put and release run in task context,
 * outside MM/spinlocks and after the mapping's TLB retirement. The callback
 * and its context must remain valid through that final reference. */
struct mm_mapping_owner {
    refcount_t references;
    void (*release)(struct mm_mapping_owner *);
};

static inline void mm_mapping_owner_init(struct mm_mapping_owner *owner,
                                          void (*release)(struct mm_mapping_owner *)) {
    refcount_set(&owner->references, 1);
    owner->release = release;
}
static inline void mm_mapping_owner_get(struct mm_mapping_owner *owner) {
    if (owner) refcount_inc(&owner->references);
}
static inline void mm_mapping_owner_put(struct mm_mapping_owner *owner) {
    if (owner && refcount_dec_and_test(&owner->references) && owner->release)
        owner->release(owner);
}

#endif
