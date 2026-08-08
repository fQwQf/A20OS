/*
 * Global native-object statistics (docs/hybrid-kernel/02-mainstream-plan.md
 * M2): live counters for leak auditing.  Exposed read-only at
 * /proc/a20/objects; a supervisor crash/heal loop must return them to the
 * baseline exactly.
 */
#ifndef _IPC_OBJSTATS_H
#define _IPC_OBJSTATS_H

#include "core/types.h"

typedef struct a20_objstats {
    _Atomic uint64_t handles;      /* installed native handle entries */
    _Atomic uint64_t channel_eps;
    _Atomic uint64_t eventqs;
    _Atomic uint64_t vmos;
    _Atomic uint64_t vmo_pages;    /* materialized VMO frames */
    _Atomic uint64_t irq_bindings;
    _Atomic uint64_t vfiles;       /* live file objects (fdtable + fd-backed handles) */
    /* Cumulative (never decrease); not part of the leak baseline. */
    _Atomic uint64_t vmo_dirty_frames; /* VMO pages whose fresh frame held stale data */
} a20_objstats_t;

extern a20_objstats_t g_a20_objstats;

static inline void a20_objstat_add(_Atomic uint64_t *field, int64_t delta)
{
    __atomic_fetch_add(field, delta, __ATOMIC_RELAXED);
}

#endif
