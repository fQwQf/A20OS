#ifndef _CORE_PSI_H
#define _CORE_PSI_H

#include "core/types.h"

/*
 * A20OS PSI (pressure stall information) for /proc/pressure files.
 *
 * The CPU "some" metric is accounted from real scheduler contention: at each
 * timer tick a stall sample is taken where "some" is true when more runnable
 * tasks exist than online CPUs (so at least one task is waiting for the CPU).
 * The 10s/60s/300s averages are exponential moving averages of that sample;
 * "total" accumulates stall microseconds.  Memory and I/O pressure are not
 * instrumented with stall sources, so their files report the accounted
 * baseline (no stalls) in the same "some" line format.
 */

/* Called once per timer tick. */
void psi_tick(void);

/* Render one /proc/pressure/<resource> "some" line. */
void psi_render_cpu(char *buf, size_t bufsz);
void psi_render_mem(char *buf, size_t bufsz);
void psi_render_io(char *buf, size_t bufsz);

#endif /* _CORE_PSI_H */
