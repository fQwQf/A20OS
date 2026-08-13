#ifndef _CORE_PERF_H
#define _CORE_PERF_H

#include "core/cpu.h"

#define A20_PERF_MAX_CPUS 32

typedef enum a20_perf_counter {
    A20_PERF_VMA_LOOKUPS,
    A20_PERF_VMA_LOOKUP_STEPS,
    A20_PERF_PAGE_CACHE_SCAN_CALLS,
    A20_PERF_PAGE_CACHE_SCAN_ENTRIES,
    A20_PERF_PAGE_CACHE_READAHEAD_CALLS,
    A20_PERF_PAGE_CACHE_READAHEAD_PAGES,
    A20_PERF_PAGE_CACHE_WRITEBACK_BATCHES,
    A20_PERF_PAGE_CACHE_WRITEBACK_PAGES,
    A20_PERF_PAGE_CACHE_PRESSURE_BATCHES,
    A20_PERF_PAGE_CACHE_PRESSURE_PAGES,
    A20_PERF_PAGE_CACHE_DISCARDED_DIRTY,
    A20_PERF_VFS_TIME_META_CALLS,
    A20_PERF_VFS_TIME_META_PROBES,
    A20_PERF_EXT4_GROUP_PROBES,
    A20_PERF_EXT4_BITMAP_PROBES,
    A20_PERF_EXT4_BITMAP_BYTE_LOADS,
    A20_PERF_EXT4_VCACHE_HITS,
    A20_PERF_EXT4_VCACHE_MISSES,
    A20_PERF_EXT4_VCACHE_INSERTS,
    A20_PERF_EXT4_VCACHE_FULL,
    A20_PERF_PCACHE_FILL_MISSES,
    A20_PERF_PCACHE_FILL_CONTENDED,
    A20_PERF_PCACHE_FULL_OVERWRITE_SKIPS,
    A20_PERF_PCACHE_WRITEBACK_IOS,
    A20_PERF_PCACHE_WRITEBACK_PAGES,
    A20_PERF_PCACHE_WRITE_UPDATES,
    A20_PERF_PCACHE_WRITE_UPDATE_PAGES,
    A20_PERF_VFS_PERMISSION_FASTPATHS,
    A20_PERF_MM_TLB_TRANSACTIONS,
    /* Invalidation transactions requesting a local, global, or remote
     * shootdown. Remote target CPUs are counted separately and exactly. */
    A20_PERF_MM_TLB_TRANSACTION_FLUSHES,
    A20_PERF_MM_TLB_REMOTE_CPUS,
    A20_PERF_MM_DEMAND_FAULTS,
    A20_PERF_MM_FILE_FAULTS,
    A20_PERF_MM_ANON_FAULTS,
    A20_PERF_MM_ANON_BATCH_WINDOWS,
    A20_PERF_MM_ANON_BATCH_PAGES,
    A20_PERF_MM_COW_FAULTS,
    A20_PERF_VIRTIO_BLK_POLLS,
    A20_PERF_VIRTIO_BLK_ACTIVE_POLLS,
    A20_PERF_VIRTIO_BLK_USED_CHECKS,
    A20_PERF_VIRTIO_BLK_COMPLETIONS,
    A20_PERF_VIRTIO_BLK_DIRECT_DMAS,
    A20_PERF_VIRTIO_BLK_BOUNCE_DMAS,
    A20_PERF_VIRTIO_BLK_BOUNCE_BYTES,
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

/* System-wide software counters backing perf_event_open(2)
 * PERF_COUNT_SW_{PAGE_FAULTS,PAGE_FAULTS_MAJ,CONTEXT_SWITCHES}. */
extern uint64_t g_perf_sw_page_faults;
extern uint64_t g_perf_sw_page_faults_maj;
extern uint64_t g_perf_sw_context_switches;

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
