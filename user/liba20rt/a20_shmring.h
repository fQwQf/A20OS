/*
 * a20_shmring — SPSC byte-stream ring over a shared VMO.
 *
 * Design reference: docs/hybrid-kernel/00-design.md §4.2 (data plane).
 *
 * The ring lives entirely in a shared VMO so two tasks mapping the VMO at
 * different virtual addresses share the same physical frames.  All indices
 * are relative offsets, never pointers.  Synchronization is acquire/release
 * atomics; sleeping uses the native futex (keyed on the physical page, so
 * it works across processes) with the classic Dekker doorbell:
 *
 *   waiter: store flag=1 (release) -> re-check condition -> futex_wait(flag,1)
 *   waker:  advance cursor (release) -> if flag: flag=0 + futex_wake(flag,1)
 *
 * Either the waiter observes the new cursor, or the waker observes the
 * flag; one of the two always wins, so no wakeup is lost.
 *
 * Normal-path cost: zero syscalls while the ring is neither full nor empty.
 */
#ifndef _A20_SHMRING_H
#define _A20_SHMRING_H

#include "a20_types.h"
#include "a20_syscall.h"
#include "a20_sync.h"

#define A20_SHMRING_MAGIC   0x53524E47u /* "SRNG" */

typedef struct a20_shmring {
    _Atomic uint32_t magic;
    uint32_t         cap;        /* data bytes, power of two */
    _Atomic uint32_t tail;       /* producer cursor (monotonic) */
    _Atomic uint32_t head;       /* consumer cursor (monotonic) */
    _Atomic uint32_t c_sleep;    /* consumer doorbell (futex word) */
    _Atomic uint32_t p_sleep;    /* producer doorbell (futex word) */
    _Atomic uint32_t ready;      /* consumer attached (futex word) */
    _Atomic uint32_t done;       /* consumer finished (futex word) */
    uint32_t         total_lo;   /* optional: total bytes expected */
    uint32_t         total_hi;
    uint32_t         _pad[6];    /* keep header 64 bytes */
    uint8_t          data[];
} a20_shmring_t;

static inline void a20_shmring_init(void *base, uint32_t cap_pow2)
{
    a20_shmring_t *r = (a20_shmring_t *)base;
    r->cap = cap_pow2;
    __atomic_store_n(&r->tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->c_sleep, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->p_sleep, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->done, 0, __ATOMIC_RELAXED);
    r->total_lo = 0;
    r->total_hi = 0;
    __atomic_store_n(&r->magic, A20_SHMRING_MAGIC, __ATOMIC_RELEASE);
}

static inline int a20_shmring_attach(void *base)
{
    a20_shmring_t *r = (a20_shmring_t *)base;
    return __atomic_load_n(&r->magic, __ATOMIC_ACQUIRE) == A20_SHMRING_MAGIC ? 0 : -1;
}

/* Word-wise copy: the freestanding build has no memcpy, and byte loops are
 * an order of magnitude slower under emulation.  Requires 8-byte-aligned
 * src/dst and a multiple-of-8 length; callers chunk accordingly. */
static inline void a20_shmring_copy8(void *dst, const void *src, uint32_t n)
{
    uint64_t *d = (uint64_t *)dst;
    const uint64_t *s = (const uint64_t *)src;
    uint32_t w = n >> 3;
    for (uint32_t i = 0; i < w; i++) d[i] = s[i];
    uint32_t off = w << 3;
    for (uint32_t i = off; i < n; i++)
        ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
}

/* Block until the consumer signals ready (bounded by one futex round). */
static inline void a20_shmring_wait_ready(a20_shmring_t *r)
{
    while (__atomic_load_n(&r->ready, __ATOMIC_ACQUIRE) == 0)
        a20_futex_wait((uint32_t *)&r->ready, 0, A20_TIMEOUT_INFINITE);
}

static inline void a20_shmring_signal_ready(a20_shmring_t *r)
{
    __atomic_store_n(&r->ready, 1, __ATOMIC_RELEASE);
    uint32_t w;
    a20_futex_wake((uint32_t *)&r->ready, 1, &w);
}

static inline void a20_shmring_signal_done(a20_shmring_t *r)
{
    __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
    uint32_t w;
    a20_futex_wake((uint32_t *)&r->done, 1, &w);
}

static inline void a20_shmring_wait_done(a20_shmring_t *r)
{
    while (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE) == 0)
        a20_futex_wait((uint32_t *)&r->done, 0, A20_TIMEOUT_INFINITE);
}

/* Write the whole buffer, sleeping only while the ring is full. */
static inline void a20_shmring_write(a20_shmring_t *r, const void *src, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)src;
    uint32_t mask = r->cap - 1;
    while (len) {
        uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
        uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint32_t space = r->cap - (tail - head);
        if (space == 0) {
            __atomic_store_n(&r->p_sleep, 1, __ATOMIC_RELEASE);
            head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
            if (r->cap - (tail - head) == 0)
                a20_futex_wait((uint32_t *)&r->p_sleep, 1, A20_TIMEOUT_INFINITE);
            continue;
        }
        uint32_t n = len < space ? len : space;
        uint32_t idx = tail & mask;
        uint32_t c = n < r->cap - idx ? n : r->cap - idx;
        a20_shmring_copy8(r->data + idx, p, c);
        if (n > c)
            a20_shmring_copy8(r->data, p + c, n - c);
        __atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
        p += n;
        len -= n;
        if (__atomic_load_n(&r->c_sleep, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&r->c_sleep, 0, __ATOMIC_RELEASE);
            uint32_t w;
            a20_futex_wake((uint32_t *)&r->c_sleep, 1, &w);
        }
    }
}

/* Read up to len bytes; blocks until at least one byte is available. */
static inline uint32_t a20_shmring_read(a20_shmring_t *r, void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t mask = r->cap - 1;
    for (;;) {
        uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
        uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        uint32_t avail = tail - head;
        if (avail) {
            uint32_t n = len < avail ? len : avail;
            uint32_t idx = head & mask;
            uint32_t c = n < r->cap - idx ? n : r->cap - idx;
            a20_shmring_copy8(d, r->data + idx, c);
            if (n > c)
                a20_shmring_copy8(d + c, r->data, n - c);
            __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
            if (__atomic_load_n(&r->p_sleep, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&r->p_sleep, 0, __ATOMIC_RELEASE);
                uint32_t w;
                a20_futex_wake((uint32_t *)&r->p_sleep, 1, &w);
            }
            return n;
        }
        __atomic_store_n(&r->c_sleep, 1, __ATOMIC_RELEASE);
        tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        if (tail != head) {
            __atomic_store_n(&r->c_sleep, 0, __ATOMIC_RELAXED);
            continue;
        }
        a20_futex_wait((uint32_t *)&r->c_sleep, 1, A20_TIMEOUT_INFINITE);
    }
}

#endif
