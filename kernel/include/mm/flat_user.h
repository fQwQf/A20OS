#ifndef A20_MM_FLAT_USER_H
#define A20_MM_FLAT_USER_H
#include "core/types.h"
#include "core/lock.h"

/* Portable ownership for trusted NOMMU programs. This tracks legal syscall
 * ranges; it does not provide hardware isolation. Owner lifetime must cover
 * every caller/pin. A wait lease prevents unmap/address reuse until finish. */
typedef struct a20_flat_region {
    struct a20_flat_region *next;
    uintptr_t base;
    size_t size;
    void *allocation;
    unsigned pins;
} a20_flat_region_t;
typedef struct a20_flat_space {
    spinlock_t lock;
    a20_flat_region_t *regions;
    unsigned closed;
} a20_flat_space_t;
void a20_flat_init(a20_flat_space_t *space);
int a20_flat_add_fixed(a20_flat_space_t *space, a20_flat_region_t *region,
                       uintptr_t base, size_t size);
int a20_flat_alloc(a20_flat_space_t *space, size_t size, uintptr_t *out);
int a20_flat_unmap(a20_flat_space_t *space, uintptr_t base, size_t size);
int a20_flat_destroy(a20_flat_space_t *space);
/* Caller holds space->lock; no pointer escapes without that lock or a pin. */
int a20_flat_range_locked(a20_flat_space_t *space, uintptr_t base, size_t size);
a20_flat_region_t *a20_flat_pin(a20_flat_space_t *space, uintptr_t base,
                                size_t size);
void a20_flat_unpin(a20_flat_space_t *space, a20_flat_region_t *region);
int a20_flat_copy(a20_flat_space_t *space, void *kernel, void *user,
                  size_t size, int to_user);
#endif
