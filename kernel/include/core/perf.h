#ifndef _CORE_PERF_H
#define _CORE_PERF_H

#include "core/cpu.h"

#define A20_PERF_MAX_CPUS 32

typedef enum a20_perf_counter {
    A20_PERF_PAGE_CACHE_SCAN_CALLS,
    A20_PERF_PAGE_CACHE_SCAN_ENTRIES,
    A20_PERF_VFS_TIME_META_CALLS,
    A20_PERF_VFS_TIME_META_PROBES,
    A20_PERF_EXT4_GROUP_PROBES,
    A20_PERF_EXT4_BITMAP_PROBES,
    A20_PERF_MM_TLB_TRANSACTIONS,
    /* Invalidation transactions requesting a local, global, or remote
     * shootdown. Remote target CPUs are counted separately and exactly. */
    A20_PERF_MM_TLB_TRANSACTION_FLUSHES,
    A20_PERF_MM_TLB_REMOTE_CPUS,
    A20_PERF_VIRTIO_BLK_POLLS,
    A20_PERF_VIRTIO_BLK_ACTIVE_POLLS,
    A20_PERF_VIRTIO_BLK_USED_CHECKS,
    A20_PERF_VIRTIO_BLK_COMPLETIONS,
    A20_PERF_IDLE_WAIT_ATTEMPTS,
    A20_PERF_IDLE_WAIT_ENTRIES,
    A20_PERF_IDLE_WAIT_WAKE_RETURNS,
    A20_PERF_COUNTER_COUNT,
} a20_perf_counter_t;

typedef struct a20_perf_cpu_counters {
    uint64_t values[A20_PERF_COUNTER_COUNT];
} __attribute__((aligned(64))) a20_perf_cpu_counters_t;

extern uint32_t g_a20_perf_enabled;
extern a20_perf_cpu_counters_t g_a20_perf_percpu[A20_PERF_MAX_CPUS];

static inline void a20_perf_add(a20_perf_counter_t counter, uint64_t value)
{
    if (__builtin_expect(
            __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED) == 0, 1))
        return;
    if (value == 0)
        return;
    unsigned cpu = arch_current_cpu_id();
    if (cpu >= A20_PERF_MAX_CPUS)
        cpu = 0;
    __atomic_fetch_add(&g_a20_perf_percpu[cpu].values[counter], value,
                       __ATOMIC_RELAXED);
}

static inline void a20_perf_count(a20_perf_counter_t counter)
{
    a20_perf_add(counter, 1);
}

size_t a20_perf_format(char *buf, size_t bufsz);

#endif /* _CORE_PERF_H */
