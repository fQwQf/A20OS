#include "core/perf.h"
#include "core/stdio.h"

uint32_t g_a20_perf_enabled;
a20_perf_cpu_counters_t g_a20_perf_percpu[A20_PERF_MAX_CPUS];

static const char *const g_a20_perf_names[A20_PERF_COUNTER_COUNT] = {
    [A20_PERF_PAGE_CACHE_SCAN_CALLS] = "page_cache_scan_calls",
    [A20_PERF_PAGE_CACHE_SCAN_ENTRIES] = "page_cache_scan_entries",
    [A20_PERF_VFS_TIME_META_CALLS] = "vfs_time_meta_calls",
    [A20_PERF_VFS_TIME_META_PROBES] = "vfs_time_meta_probes",
    [A20_PERF_EXT4_GROUP_PROBES] = "ext4_group_probes",
    [A20_PERF_EXT4_BITMAP_PROBES] = "ext4_bitmap_probes",
    [A20_PERF_MM_TLB_TRANSACTIONS] = "mm_tlb_transactions",
    [A20_PERF_MM_TLB_TRANSACTION_FLUSHES] =
        "mm_tlb_transaction_flushes",
    [A20_PERF_MM_TLB_REMOTE_CPUS] = "mm_tlb_remote_cpus",
    [A20_PERF_VIRTIO_BLK_POLLS] = "virtio_blk_polls",
    [A20_PERF_VIRTIO_BLK_ACTIVE_POLLS] = "virtio_blk_active_polls",
    [A20_PERF_VIRTIO_BLK_USED_CHECKS] = "virtio_blk_used_checks",
    [A20_PERF_VIRTIO_BLK_COMPLETIONS] = "virtio_blk_completions",
    [A20_PERF_IDLE_WAIT_ATTEMPTS] = "idle_wait_attempts",
    [A20_PERF_IDLE_WAIT_ENTRIES] = "idle_wait_entries",
    [A20_PERF_IDLE_WAIT_WAKE_RETURNS] = "idle_wait_wake_returns",
};

size_t a20_perf_format(char *buf, size_t bufsz)
{
    size_t off = 0;

    /* Collection is dormant during formal timed builds. The first diagnostic
     * read enables cumulative accounting before taking its initial snapshot. */
    __atomic_store_n(&g_a20_perf_enabled, 1, __ATOMIC_RELEASE);

    for (unsigned counter = 0; counter < A20_PERF_COUNTER_COUNT; counter++) {
        uint64_t total = 0;
        for (unsigned cpu = 0; cpu < A20_PERF_MAX_CPUS; cpu++)
            total += __atomic_load_n(&g_a20_perf_percpu[cpu].values[counter],
                                     __ATOMIC_RELAXED);
        if (off >= bufsz)
            break;
        int n = snprintf(buf + off, bufsz - off, "%s: %lu\n",
                         g_a20_perf_names[counter], (unsigned long)total);
        if (n < 0)
            break;
        if ((size_t)n >= bufsz - off) {
            off = bufsz ? bufsz - 1 : 0;
            break;
        }
        off += (size_t)n;
    }
    return off;
}
