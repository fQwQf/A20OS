#if defined(CONFIG_FLAT_USER_MEMORY) || defined(A20_FLAT_HOST_TEST)
#include "mm/flat_user.h"
#include "mm/slab.h"
#include "core/consts.h"
#include "core/errno.h"
#include "core/string.h"

static a20_flat_region_t *find(a20_flat_space_t *s, uintptr_t p, size_t n) {
    if (s->closed)
        return NULL;
    for (a20_flat_region_t *r = s->regions; r; r = r->next)
        if (p >= r->base && p - r->base <= r->size &&
            n <= r->size - (p - r->base))
            return r;
    return NULL;
}
static int rounded(size_t n, size_t *out) {
    if (!n || n > SIZE_MAX - (PAGE_SIZE - 1))
        return -EINVAL;
    *out = (n + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    return 0;
}
void a20_flat_init(a20_flat_space_t *s) {
    memset(s, 0, sizeof(*s));
    s->lock = (spinlock_t)SPINLOCK_INIT;
}
int a20_flat_add_fixed(a20_flat_space_t *s, a20_flat_region_t *r, uintptr_t p,
                       size_t n) {
    if (!s || !r || !p || !n || n > (uintptr_t)-1 - p)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    int result = s->closed ? -EINVAL : 0;
    for (a20_flat_region_t *v = s->regions; !result && v; v = v->next)
        if (v == r || (p < v->base + v->size && v->base < p + n))
            result = -EINVAL;
    if (!result) {
        *r = (a20_flat_region_t){.next = s->regions, .base = p, .size = n};
        s->regions = r;
    }
    spin_unlock_irqrestore(&s->lock, flags);
    return result;
}
int a20_flat_range_locked(a20_flat_space_t *s, uintptr_t p, size_t n) {
    return s && find(s, p, n) != NULL;
}
int a20_flat_alloc(a20_flat_space_t *s, size_t n, uintptr_t *out) {
    size_t size;
    if (!s || !out || rounded(n, &size) || size > SIZE_MAX - (PAGE_SIZE - 1))
        return -EINVAL;
    uint64_t preflight = spin_lock_irqsave(&s->lock);
    int closed = s->closed;
    spin_unlock_irqrestore(&s->lock, preflight);
    if (closed)
        return -EINVAL;
    a20_flat_region_t *r = kcalloc(1, sizeof(*r));
    if (!r)
        return -ENOMEM;
    r->allocation = kcalloc(1, size + PAGE_SIZE - 1);
    if (!r->allocation) {
        kfree(r);
        return -ENOMEM;
    }
    r->base = ((uintptr_t)r->allocation + PAGE_SIZE - 1) &
              ~(uintptr_t)(PAGE_SIZE - 1);
    r->size = size;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    int invalid = s->closed;
    for (a20_flat_region_t *v = s->regions; !invalid && v; v = v->next)
        if (r->base < v->base + v->size && v->base < r->base + r->size)
            invalid = 1;
    if (invalid) {
        spin_unlock_irqrestore(&s->lock, flags);
        kfree(r->allocation);
        kfree(r);
        return -EINVAL;
    }
    r->next = s->regions;
    s->regions = r;
    *out = r->base;
    spin_unlock_irqrestore(&s->lock, flags);
    return 0;
}
int a20_flat_unmap(a20_flat_space_t *s, uintptr_t p, size_t n) {
    size_t size;
    if (!s || !p || (p & (PAGE_SIZE - 1)) || rounded(n, &size))
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    a20_flat_region_t **at = &s->regions;
    while (*at && ((*at)->base != p || (*at)->size != size))
        at = &(*at)->next;
    a20_flat_region_t *r = *at;
    int result = s->closed || !r || !r->allocation ? -EINVAL
                 : r->pins                         ? -EBUSY
                                                   : 0;
    if (!result)
        *at = r->next;
    spin_unlock_irqrestore(&s->lock, flags);
    if (!result) {
        kfree(r->allocation);
        kfree(r);
    }
    return result;
}
a20_flat_region_t *a20_flat_pin(a20_flat_space_t *s, uintptr_t p, size_t n) {
    if (!s)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    a20_flat_region_t *r = find(s, p, n);
    if (r && r->pins != (unsigned)-1)
        r->pins++;
    else
        r = NULL;
    spin_unlock_irqrestore(&s->lock, flags);
    return r;
}
void a20_flat_unpin(a20_flat_space_t *s, a20_flat_region_t *r) {
    uint64_t flags = spin_lock_irqsave(&s->lock);
    if (r->pins)
        r->pins--;
    spin_unlock_irqrestore(&s->lock, flags);
}
int a20_flat_copy(a20_flat_space_t *s, void *kernel, void *user, size_t n,
                  int to_user) {
    if (!s || (n && !kernel))
        return -EFAULT;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    int result = find(s, (uintptr_t)user, n) ? 0 : -EFAULT;
    if (!result && n) {
        if (to_user)
            memcpy(user, kernel, n);
        else
            memcpy(kernel, user, n);
    }
    spin_unlock_irqrestore(&s->lock, flags);
    return result;
}
int a20_flat_destroy(a20_flat_space_t *s) {
    if (!s)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    for (a20_flat_region_t *r = s->regions; r; r = r->next)
        if (r->pins) {
            spin_unlock_irqrestore(&s->lock, flags);
            return -EBUSY;
        }
    a20_flat_region_t *list = s->regions;
    s->regions = NULL;
    s->closed = 1;
    spin_unlock_irqrestore(&s->lock, flags);
    while (list) {
        a20_flat_region_t *next = list->next;
        if (list->allocation) {
            kfree(list->allocation);
            kfree(list);
        }
        list = next;
    }
    return 0;
}
#endif
