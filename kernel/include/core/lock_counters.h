#ifndef _CORE_LOCK_COUNTERS_H
#define _CORE_LOCK_COUNTERS_H

#include "core/types.h"

typedef struct spinlock spinlock_t;

/* Register a spinlock for contention telemetry.  idempotent; the same lock
 * may be registered from several subsystem inits. */
void lock_counters_register(spinlock_t *lock, const char *name);

/* Allocate the per-callsite sample table for one lock so the contended path
 * records the caller's return address.  Only call for the locks you want to
 * attribute (e.g. the scheduler's proc_lock). */
void lock_counters_enable_callsite(spinlock_t *lock);

/* Render "<name>: <contended_acquires> <contended_spins>\n" for every
 * registered lock.  Returns bytes written. */
size_t lock_counters_format(char *buf, size_t bufsz);

void lock_counters_init(void);

#endif /* _CORE_LOCK_COUNTERS_H */
