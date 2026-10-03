#ifndef _CORE_SEQLOCK_H
#define _CORE_SEQLOCK_H

#include "core/types.h"

/*
 * Seqlock: a writer-serialised table that readers scan without taking a lock.
 *
 * The case it exists for is a reader on a hot path that already holds a
 * different lock and cannot take the writer's. Taking a lock there would
 * serialise the packet path behind configuration edits; a seqlock leaves the
 * reader wait-free instead. kernel/net/netfilter.c is the worked example: the
 * rule table is written under g_netfilter_lock while the packet path scans it
 * holding g_lwip_lock.
 *
 * It is a *retry* primitive, not a *consistency* primitive. A reader that
 * detects a concurrent write must discard everything it read and start again;
 * a torn intermediate value must never be acted on. That is why every accessor
 * here comes in a begin/retry pair rather than a single "read" call, and why
 * the payload must be reachable again on the second pass.
 *
 * Writers must be serialised against each other by the caller -- typically by
 * holding the spinlock that owns the table. This kernel has no RCU and no
 * preemption model to fall back on, so a bare pair of these does not exclude
 * concurrent writers. The sequence counter detects overlap, it does not
 * prevent it.
 */
typedef struct seqlock {
    unsigned seq;
} seqlock_t;

#define SEQLOCK_INIT { 0u }

/*
 * Writer side.  The counter is odd while the writer is inside the critical
 * section, so a reader that samples an odd value knows to back off. The first
 * bump is acquire-release so the caller's prior writes cannot be reordered
 * after it; the second is release only, because it is the store that publishes
 * the payload.
 */
static inline void seqlock_write_begin(seqlock_t *sl)
{
    __atomic_add_fetch(&sl->seq, 1u, __ATOMIC_ACQ_REL);
}

static inline void seqlock_write_end(seqlock_t *sl)
{
    __atomic_add_fetch(&sl->seq, 1u, __ATOMIC_RELEASE);
}

/*
 * Reader side.  seqlock_read_begin() returns 0 when a writer is inside, which
 * is deliberately also a value no successful sample can produce, so a single
 * comparison covers "odd" and "not a valid sample".
 */
static inline unsigned seqlock_read_begin(const seqlock_t *sl)
{
    unsigned s = __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE);
    return (s & 1u) ? 0u : s;
}

static inline bool seqlock_read_retry(const seqlock_t *sl, unsigned start)
{
    return __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE) != start;
}

/*
 * Iteration bound.  A writer that is preempted for long enough can otherwise
 * spin a reader indefinitely; a bound turns that into a stale-but-consistent
 * answer, and it is the caller's job to decide what that answer means. An
 * unconfigured filter, for instance, wants the same answer it would give with
 * no rules at all, which is fail-open -- see the note in netfilter.c.
 */
#define SEQLOCK_READ_ATTEMPTS 4

#endif /* _CORE_SEQLOCK_H */
