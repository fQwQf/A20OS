/*
 * A20OS Native ABI — Virtual Memory Object (VMO).
 * Design reference: docs/native-abi/04-memory.md §2
 */
#ifndef _ABI_NATIVE_VMO_H
#define _ABI_NATIVE_VMO_H

#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "mm/frame.h"

struct a20_vmo {
    refcount_t  refcount;
    uint64_t    size;
    uint64_t    phys_size;
    uint32_t    type;
    uint32_t    options;
    spinlock_t  lock;
    pfn_t      *pages;
    uint32_t    page_count;
};

typedef struct a20_vmo a20_vmo_t;

struct a20_vmo *a20_vmo_create(uint32_t type, uint64_t size, uint32_t options);
void            a20_vmo_destroy(struct a20_vmo *vmo);
void            a20_vmo_release(struct a20_vmo *vmo);
pfn_t           a20_vmo_get_page(struct a20_vmo *vmo, uint32_t index);
int64_t         a20_vmo_resize(struct a20_vmo *vmo, uint64_t new_size);

static inline void a20_vmo_ref(struct a20_vmo *vmo)
{
    if (vmo) refcount_inc(&vmo->refcount);
}

#endif
