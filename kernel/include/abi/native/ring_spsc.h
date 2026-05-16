/*
 * A20 lock-free SPSC ring buffer — single-producer single-consumer
 * event queue backend.
 *
 * Uses atomic indices instead of locks. Producer owns write_idx,
 * consumer owns read_idx. Memory ordering via __ATOMIC_RELEASE/ACQUIRE
 * ensures visibility.
 *
 * For multi-producer or multi-consumer, fall back to locked ring.
 */

#ifndef _A20_RING_SPSC_H
#define _A20_RING_SPSC_H

#include <stdint.h>
#include <stddef.h>

struct a20_ring_spsc {
    void    **buf;
    uint32_t  mask;       /* capacity - 1 (must be power-of-2) */
    uint32_t  _pad;
    _Atomic uint32_t write_idx;
    _Atomic uint32_t read_idx;
};

static inline void a20_ring_spsc_init(struct a20_ring_spsc *r,
                                       void **buf, uint32_t capacity)
{
    r->buf      = buf;
    r->mask     = capacity - 1;
    r->write_idx = 0;
    r->read_idx  = 0;
}

static inline uint32_t a20_ring_spsc_capacity(struct a20_ring_spsc *r)
{
    return r->mask + 1;
}

static inline uint32_t a20_ring_spsc_used(struct a20_ring_spsc *r)
{
    uint32_t w = __atomic_load_n(&r->write_idx, __ATOMIC_RELAXED);
    uint32_t rd = __atomic_load_n(&r->read_idx, __ATOMIC_RELAXED);
    return (w - rd) & r->mask;
}

static inline int a20_ring_spsc_is_full(struct a20_ring_spsc *r)
{
    return a20_ring_spsc_used(r) == r->mask;
}

static inline int a20_ring_spsc_is_empty(struct a20_ring_spsc *r)
{
    uint32_t w = __atomic_load_n(&r->write_idx, __ATOMIC_RELAXED);
    uint32_t rd = __atomic_load_n(&r->read_idx, __ATOMIC_ACQUIRE);
    return w == rd;
}

static inline int a20_ring_spsc_push(struct a20_ring_spsc *r, void *item)
{
    uint32_t w = __atomic_load_n(&r->write_idx, __ATOMIC_RELAXED);
    uint32_t rd = __atomic_load_n(&r->read_idx, __ATOMIC_ACQUIRE);
    if (((w + 1) & r->mask) == rd) return -1; /* full */

    r->buf[w & r->mask] = item;
    __atomic_store_n(&r->write_idx, (w + 1) & r->mask, __ATOMIC_RELEASE);
    return 0;
}

static inline void *a20_ring_spsc_pop(struct a20_ring_spsc *r)
{
    uint32_t rd = __atomic_load_n(&r->read_idx, __ATOMIC_RELAXED);
    uint32_t w = __atomic_load_n(&r->write_idx, __ATOMIC_ACQUIRE);
    if (rd == w) return NULL; /* empty */

    void *item = r->buf[rd & r->mask];
    __atomic_store_n(&r->read_idx, (rd + 1) & r->mask, __ATOMIC_RELEASE);
    return item;
}

static inline uint32_t a20_ring_spsc_push_batch(struct a20_ring_spsc *r,
                                                  void **items, uint32_t n)
{
    uint32_t pushed = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (a20_ring_spsc_push(r, items[i]) != 0) break;
        pushed++;
    }
    return pushed;
}

static inline uint32_t a20_ring_spsc_pop_batch(struct a20_ring_spsc *r,
                                                 void **items, uint32_t n)
{
    uint32_t popped = 0;
    for (uint32_t i = 0; i < n; i++) {
        void *item = a20_ring_spsc_pop(r);
        if (!item) break;
        items[i] = item;
        popped++;
    }
    return popped;
}

#endif /* _A20_RING_SPSC_H */
