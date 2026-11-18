/*
 * Global native-object statistics (docs/zh/hybrid-kernel/02-mainstream-plan.md
 * M2): live counters for leak auditing.  Exposed read-only at
 * /proc/a20/objects; a supervisor crash/heal loop must return them to the
 * baseline exactly.
 */
#ifndef _IPC_OBJSTATS_H
#define _IPC_OBJSTATS_H

#include "core/types.h"
#ifdef CONFIG_UP_SOFT_ATOMICS
#include "core/arch.h"
#endif

/* Access these counters through __atomic builtins (or the UP IRQ guard).
 * Plain storage is required by Clang's GNU __atomic interface. */
typedef struct a20_objstats {
    uint64_t handles;      /* installed native handle entries */
    uint64_t channel_eps;
    uint64_t eventqs;
    uint64_t vmos;
    uint64_t vmo_pages;    /* materialized VMO frames */
    uint64_t irq_bindings;
    uint64_t vfiles;       /* live file objects (fdtable + fd-backed handles) */
    /* Cumulative (never decrease); not part of the leak baseline. */
    uint64_t vmo_dirty_frames; /* VMO pages whose fresh frame held stale data */
} a20_objstats_t;

extern a20_objstats_t g_a20_objstats;

static inline void a20_objstat_add(uint64_t *field, int64_t delta)
{
#ifdef CONFIG_UP_SOFT_ATOMICS
    /* A single core with no 64-bit atomics: IRQ masking makes the
     * read-modify-write of the counter indivisible. */
    uint32_t flags = arch_irq_save();
    *(uint64_t *)field = *(uint64_t *)field + (uint64_t)delta;
    arch_irq_restore(flags);
#else
    __atomic_fetch_add(field, delta, __ATOMIC_RELAXED);
#endif
}

#endif
