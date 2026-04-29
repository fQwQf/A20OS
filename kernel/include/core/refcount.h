#ifndef _CORE_REFCOUNT_H
#define _CORE_REFCOUNT_H

#include "core/types.h"

typedef struct refcount {
    volatile int value;
} refcount_t;

#define REFCOUNT_INIT(n) { (n) }

static inline void refcount_set(refcount_t *r, int v) {
    __atomic_store_n(&r->value, v, __ATOMIC_RELAXED);
}

static inline int refcount_read(const refcount_t *r) {
    return __atomic_load_n(&r->value, __ATOMIC_RELAXED);
}

static inline void refcount_inc(refcount_t *r) {
    __atomic_add_fetch(&r->value, 1, __ATOMIC_RELAXED);
}

static inline int refcount_inc_not_zero(refcount_t *r) {
    int old = __atomic_load_n(&r->value, __ATOMIC_RELAXED);
    while (old != 0) {
        if (__atomic_compare_exchange_n(&r->value, &old, old + 1, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

static inline int refcount_dec_and_test(refcount_t *r) {
    return __atomic_sub_fetch(&r->value, 1, __ATOMIC_ACQ_REL) == 0;
}

#endif
