#if defined(CONFIG_FLAT_USER_MEMORY) || defined(A20_FLAT_HOST_TEST)
#include "mm/flat_user.h"
#include "mm/slab.h"
#include "core/consts.h"
#include "core/errno.h"
#include "core/string.h"

/*
 * The flat-space decisions live in kernel/mm/flat_policy.rs.  What is left here
 * is the region and space layout and the list surgery, the spinlock, the
 * allocator and the copy.  None of the bridges decides anything.
 */
extern void a20_flat_init(a20_flat_space_t *s);
extern int a20_flat_add_fixed(a20_flat_space_t *s, a20_flat_region_t *r,
                              uintptr_t p, size_t n);
extern int a20_flat_range_locked(a20_flat_space_t *s, uintptr_t p, size_t n);
extern int a20_flat_alloc(a20_flat_space_t *s, size_t n, uintptr_t *out);
extern int a20_flat_unmap(a20_flat_space_t *s, uintptr_t p, size_t n);
extern a20_flat_region_t *a20_flat_pin(a20_flat_space_t *s, uintptr_t p, size_t n);
extern void a20_flat_unpin(a20_flat_space_t *s, a20_flat_region_t *r);
extern int a20_flat_copy(a20_flat_space_t *s, void *kernel, void *user,
                         size_t n, int to_user);
extern int a20_flat_destroy(a20_flat_space_t *s);

void a20_flat_init_space(a20_flat_space_t *s)
{
    memset(s, 0, sizeof(*s));
    s->lock = (spinlock_t)SPINLOCK_INIT;
}

uint64_t a20_flat_lock(a20_flat_space_t *s)
{
    return spin_lock_irqsave(&s->lock);
}

void a20_flat_unlock(a20_flat_space_t *s, uint64_t flags)
{
    spin_unlock_irqrestore(&s->lock, flags);
}

int a20_flat_is_closed(const a20_flat_space_t *s)
{
    return s->closed ? 1 : 0;
}

void a20_flat_set_closed(a20_flat_space_t *s)
{
    s->closed = 1;
}

a20_flat_region_t *a20_flat_head(const a20_flat_space_t *s)
{
    return s->regions;
}

void a20_flat_set_head(a20_flat_space_t *s, a20_flat_region_t *r)
{
    s->regions = r;
}

a20_flat_region_t *a20_flat_next(const a20_flat_region_t *r)
{
    return r->next;
}

void a20_flat_set_next(a20_flat_region_t *r, a20_flat_region_t *next)
{
    r->next = next;
}

void a20_flat_region_fields(const a20_flat_region_t *r, uintptr_t *base,
                            size_t *size, void **allocation, unsigned *pins)
{
    if (base) *base = r->base;
    if (size) *size = r->size;
    if (allocation) *allocation = r->allocation;
    if (pins) *pins = r->pins;
}

void a20_flat_set_region(a20_flat_region_t *r, a20_flat_region_t *next,
                         uintptr_t base, size_t size, void *allocation)
{
    r->next = next;
    r->base = base;
    r->size = size;
    r->allocation = allocation;
    r->pins = 0;
}

void a20_flat_set_pins(a20_flat_region_t *r, unsigned pins)
{
    r->pins = pins;
}

a20_flat_region_t *a20_flat_region_alloc(void)
{
    return kcalloc(1, sizeof(a20_flat_region_t));
}

void *a20_flat_backing_alloc(size_t size)
{
    return kcalloc(1, size + PAGE_SIZE - 1);
}

void a20_flat_kfree(void *p)
{
    kfree(p);
}

void a20_flat_copy_bytes(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
}


#endif /* CONFIG_FLAT_USER_MEMORY || A20_FLAT_HOST_TEST */
