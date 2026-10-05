#ifndef _CORE_LOCK_COUNTERS_H
#define _CORE_LOCK_COUNTERS_H

#include "core/types.h"

typedef struct spinlock spinlock_t;

/* Register a spinlock for contention telemetry.  idempotent; the same lock
 * may be registered from several subsystem inits. */
void lock_counters_register(spinlock_t *lock, const char *name);

/* Allocate the per-callsite sample table for one lock so the contended path
 * records the caller's return address.  Only call for the locks you want to
 * attribute (e.g. the scheduler's tasklist_lock). */
void lock_counters_enable_callsite(spinlock_t *lock);

/* Render "<name>: <contended_acquires> <contended_spins>\n" for every
 * registered lock.  Returns bytes written. */
size_t lock_counters_format(char *buf, size_t bufsz);

void lock_counters_init(void);

/*
 * How many locks the fixed registry currently holds, and how many
 * lock_counters_register() calls it had to refuse because it was full.
 *
 * impl-notes-net.md §8.2 and §8.7 recorded the budget question -- the server
 * profile registers 128 socket-table bucket locks plus proc, runq, lwip, the
 * three fs locks and the slab cache, against LOCK_COUNTERS_MAX of 192 -- and
 * noted that a registration past the ceiling was *silently* dropped.  A silent
 * drop is the one failure mode a contention audit cannot detect: the lock still
 * works, the lock just stops being visible, and the report reads clean.  These
 * two accessors make the drop visible instead of inferable.
 */
unsigned lock_counters_count(void);
unsigned lock_counters_dropped(void);

/* Zero the counters of every registered lock, keeping the registrations (and
 * the allocated per-callsite tables) intact.  Lets one run own its measurement
 * window instead of inferring it by subtracting two cumulative reads. */
void lock_counters_reset(void);

#endif /* _CORE_LOCK_COUNTERS_H */
