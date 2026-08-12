#include "core/perf.h"
#include "core/stdio.h"
#include "sys/syscall.h"

uint32_t g_a20_perf_enabled;
a20_perf_cpu_counters_t g_a20_perf_percpu[A20_PERF_MAX_CPUS];

static const char *const g_a20_perf_names[A20_PERF_COUNTER_COUNT] = {
    [A20_PERF_VMA_LOOKUPS] = "vma_lookups",
    [A20_PERF_VMA_LOOKUP_STEPS] = "vma_lookup_steps",
    [A20_PERF_PAGE_CACHE_SCAN_CALLS] = "page_cache_scan_calls",
    [A20_PERF_PAGE_CACHE_SCAN_ENTRIES] = "page_cache_scan_entries",
    [A20_PERF_PAGE_CACHE_READAHEAD_CALLS] =
        "page_cache_readahead_calls",
    [A20_PERF_PAGE_CACHE_READAHEAD_PAGES] =
        "page_cache_readahead_pages",
    [A20_PERF_PAGE_CACHE_WRITEBACK_BATCHES] =
        "page_cache_writeback_batches",
    [A20_PERF_PAGE_CACHE_WRITEBACK_PAGES] =
        "page_cache_writeback_pages",
    [A20_PERF_PAGE_CACHE_PRESSURE_BATCHES] =
        "page_cache_pressure_batches",
    [A20_PERF_PAGE_CACHE_PRESSURE_PAGES] =
        "page_cache_pressure_pages",
    [A20_PERF_PAGE_CACHE_DISCARDED_DIRTY] =
        "page_cache_discarded_dirty",
    [A20_PERF_VFS_TIME_META_CALLS] = "vfs_time_meta_calls",
    [A20_PERF_VFS_TIME_META_PROBES] = "vfs_time_meta_probes",
    [A20_PERF_EXT4_GROUP_PROBES] = "ext4_group_probes",
    [A20_PERF_EXT4_BITMAP_PROBES] = "ext4_bitmap_probes",
    [A20_PERF_EXT4_BITMAP_BYTE_LOADS] = "ext4_bitmap_byte_loads",
    [A20_PERF_EXT4_VCACHE_HITS] = "ext4_vcache_hits",
    [A20_PERF_EXT4_VCACHE_MISSES] = "ext4_vcache_misses",
    [A20_PERF_EXT4_VCACHE_INSERTS] = "ext4_vcache_inserts",
    [A20_PERF_EXT4_VCACHE_FULL] = "ext4_vcache_full",
    [A20_PERF_PCACHE_FILL_MISSES] = "pcache_fill_misses",
    [A20_PERF_PCACHE_FILL_CONTENDED] = "pcache_fill_contended",
    [A20_PERF_PCACHE_FULL_OVERWRITE_SKIPS] =
        "pcache_full_overwrite_skips",
    [A20_PERF_PCACHE_WRITEBACK_IOS] = "pcache_writeback_ios",
    [A20_PERF_PCACHE_WRITEBACK_PAGES] = "pcache_writeback_pages",
    [A20_PERF_PCACHE_WRITE_UPDATES] = "pcache_write_updates",
    [A20_PERF_PCACHE_WRITE_UPDATE_PAGES] =
        "pcache_write_update_pages",
    [A20_PERF_VFS_PERMISSION_FASTPATHS] =
        "vfs_permission_fastpaths",
    [A20_PERF_MM_TLB_TRANSACTIONS] = "mm_tlb_transactions",
    [A20_PERF_MM_TLB_TRANSACTION_FLUSHES] =
        "mm_tlb_transaction_flushes",
    [A20_PERF_MM_TLB_REMOTE_CPUS] = "mm_tlb_remote_cpus",
    [A20_PERF_MM_DEMAND_FAULTS] = "mm_demand_faults",
    [A20_PERF_MM_FILE_FAULTS] = "mm_file_faults",
    [A20_PERF_MM_ANON_FAULTS] = "mm_anon_faults",
    [A20_PERF_MM_ANON_BATCH_WINDOWS] = "mm_anon_batch_windows",
    [A20_PERF_MM_ANON_BATCH_PAGES] = "mm_anon_batch_pages",
    [A20_PERF_MM_COW_FAULTS] = "mm_cow_faults",
    [A20_PERF_VIRTIO_BLK_POLLS] = "virtio_blk_polls",
    [A20_PERF_VIRTIO_BLK_ACTIVE_POLLS] = "virtio_blk_active_polls",
    [A20_PERF_VIRTIO_BLK_USED_CHECKS] = "virtio_blk_used_checks",
    [A20_PERF_VIRTIO_BLK_COMPLETIONS] = "virtio_blk_completions",
    [A20_PERF_VIRTIO_BLK_DIRECT_DMAS] = "virtio_blk_direct_dmas",
    [A20_PERF_VIRTIO_BLK_BOUNCE_DMAS] = "virtio_blk_bounce_dmas",
    [A20_PERF_VIRTIO_BLK_BOUNCE_BYTES] = "virtio_blk_bounce_bytes",
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
    for (unsigned nr = 0; nr < SYSCALL_PROFILE_MAX && off < bufsz; nr++) {
        uint64_t count = __atomic_load_n(&sys_prof[nr].count,
                                         __ATOMIC_RELAXED);
        if (!count)
            continue;
        uint64_t ticks = __atomic_load_n(&sys_prof[nr].cycles,
                                         __ATOMIC_RELAXED);
        int n = snprintf(buf + off, bufsz - off,
                         "syscall_%u_count: %lu\n"
                         "syscall_%u_ticks: %lu\n",
                         nr, (unsigned long)count,
                         nr, (unsigned long)ticks);
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
