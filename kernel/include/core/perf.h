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
    A20_PERF_BLOCK_FLUSHES,
    A20_PERF_PCACHE_WRITEBACK_PAGES,
    A20_PERF_PCACHE_WRITE_UPDATES,
    A20_PERF_PCACHE_WRITE_UPDATE_PAGES,
    A20_PERF_VFS_PERMISSION_FASTPATHS,
    A20_PERF_MM_TLB_TRANSACTIONS,
    /* Invalidation transactions requesting a local, global, or remote
     * shootdown. Remote target CPUs are counted separately and exactly. */
    A20_PERF_MM_TLB_TRANSACTION_FLUSHES,
    A20_PERF_MM_TLB_REMOTE_CPUS,
    /* mm_context_enter() runs inside the selected task's park_lock critical
     * section, so its convergence loop extends how long that lock is held.
     * WAITS counts
     * enters that flushed at least once; FLUSHES - WAITS is the extra
     * re-convergence cost under live TLB writers. */
    A20_PERF_MM_CONTEXT_ENTERS,
    A20_PERF_MM_TLB_CONVERGE_WAITS,
    A20_PERF_MM_TLB_CONVERGE_FLUSHES,
    A20_PERF_MM_DEMAND_FAULTS,
    A20_PERF_MM_FILE_FAULTS,
    A20_PERF_MM_ANON_FAULTS,
    A20_PERF_MM_ANON_BATCH_WINDOWS,
    A20_PERF_MM_ANON_BATCH_PAGES,
    A20_PERF_MM_COW_FAULTS,
    /* Single-level model: page-table lock behaviour.  A cursor that finds the
     * covering node busy is contending with a transaction on an overlapping
     * range; the ratio of contended to total acquisitions is the direct
     * measure of how well disjoint ranges avoid serialising. */
    A20_PERF_MM_PT_LOCK_ACQUIRES,
    A20_PERF_MM_PT_LOCK_CONTENDED,
    A20_PERF_MM_PT_LOCK_WAITS,
    A20_PERF_MM_CURSOR_OPEN,
    A20_PERF_MM_CURSOR_STALE_RETRY,
    /*
     * Per-fault serialisation probes.  §8.18/§8.19 ruled out mm->lock, the
     * cgroup charge and memset bandwidth; these measure the two locks that
     * remain on the anonymous fault path so the real serialisation point can
     * be identified by measurement rather than by guesswork.
     */
    A20_PERF_MM_CG_LOCK_ACQUIRES,
    A20_PERF_MM_CG_LOCK_CONTENDED,
    A20_PERF_MM_PFA_LOCK_ACQUIRES,
    A20_PERF_MM_PFA_LOCK_CONTENDED,
    /* MM_AS_ANON_PROVISION: leaves marked reserved-but-not-backed at mmap.
     * The shutdown audit cannot witness this state (address spaces are gone
     * by then), so it needs its own counter to be observable. */
    A20_PERF_MM_ANON_PROVISIONED,
    /* MM_AS_FAULT_FROM_STATUS: demand faults served from per-PTE status with
     * no VMA lookup at all (paper Fig. 8). */
    A20_PERF_MM_FAULT_FROM_STATUS,
    A20_PERF_VIRTIO_BLK_POLLS,
    A20_PERF_VIRTIO_BLK_ACTIVE_POLLS,
    A20_PERF_VIRTIO_BLK_USED_CHECKS,
    A20_PERF_VIRTIO_BLK_COMPLETIONS,
    A20_PERF_VIRTIO_BLK_DIRECT_DMAS,
    A20_PERF_VIRTIO_BLK_BOUNCE_DMAS,
    A20_PERF_VIRTIO_BLK_BOUNCE_BYTES,
    /*
     * AHCI completion path.  A working interrupt path and a working polling
     * fallback are indistinguishable from the outside -- both make the disk
     * work -- so the split has to be counted explicitly.  A run that ends with
     * ahci_poll_completions carrying every command and ahci_irq_completions
     * at 0 has proven nothing about the IRQ path, whatever the driver printed.
     * ahci_commands is the denominator; one command can be seen by both a park
     * round and a poll, so the two completion counters bound it from above
     * rather than summing to it.
     */
    A20_PERF_AHCI_COMMANDS,
    A20_PERF_AHCI_IRQ_COMPLETIONS,
    A20_PERF_AHCI_IRQ_WAKEUPS,
    A20_PERF_AHCI_PARK_ROUNDS,
    A20_PERF_AHCI_POLL_COMPLETIONS,
    A20_PERF_AHCI_ERRORS,
    /*
     * E1000 interrupt data plane.  Same reasoning as the AHCI block above: an
     * e1000 whose ring moves packets proves nothing about its interrupt path,
     * because the polling hook drains the same ring from the same lwIP drain.
     *
     *   e1000_irq_calls        -- handler entries, whatever the ICR read found.
     *   e1000_irq_rx          -- entries whose ICR carried a receive cause.
     *   e1000_irq_tx          -- entries whose ICR carried a transmit-DW cause.
     *                            One entry can carry both, so irq_rx and irq_tx
     *                            bound irq_calls rather than summing to it.
     *   e1000_irq_empty       -- entries that read ICR == 0.  These are the
     *                            cost of a shared line, not a fault; a large
     *                            share is what a throttle looks like.
     *   e1000_tx_reclaimed    -- TX descriptors released back to software after
     *                            the device retired them.  Bounded by the number
     *                            of frames ever handed to send().
     *   e1000_rx_drained      -- receive descriptors this driver retired,
     *                            counted here because the drain the stack runs
     *                            is the stack's counter, not the driver's.
     */
    A20_PERF_E1000_IRQ_CALLS,
    A20_PERF_E1000_IRQ_RX,
    A20_PERF_E1000_IRQ_TX,
    A20_PERF_E1000_IRQ_EMPTY,
    A20_PERF_E1000_TX_RECLAIMED,
    A20_PERF_E1000_RX_DRAINED,
    /*
     * RTL8139 interrupt data plane.  The same gap as the E1000 block above, and
     * it is wider here: .poll reclaims transmit descriptors from the same lwIP
     * drain that runs whether or not a handler ever fires, so a working network
     * is not evidence that the INTx line reached this driver.
     *
     *   rtl8139_irq_calls      -- handler entries that found a non-zero ISR.
     *   rtl8139_irq_rx         -- entries that saw ISR.TOK, i.e. a cause that
     *                            only the receive ring can raise.
     *   rtl8139_irq_tx         -- entries that saw a transmit-retired cause.
     *   rtl8139_tx_reclaimed   -- TSD descriptors released back to send().  Kept
     *                            apart from irq_tx because that count can be
     *                            positive while the reclaim was dropped, and four
     *                            descriptors is deep enough to hide it.
     *   rtl8139_rx_drained     -- frames retired out of the RX ring, delivered
     *                            or dropped.
     */
    A20_PERF_RTL8139_IRQ_CALLS,
    A20_PERF_RTL8139_IRQ_RX,
    A20_PERF_RTL8139_IRQ_TX,
    A20_PERF_RTL8139_TX_RECLAIMED,
    A20_PERF_RTL8139_RX_DRAINED,
    A20_PERF_IDLE_WAIT_ATTEMPTS,
    A20_PERF_IDLE_WAIT_ENTRIES,
    A20_PERF_IDLE_WAIT_WAKE_RETURNS,
    /*
     * Network data-path counters.  The whole TCP/IP data plane runs under one
     * global spinlock, so the two numbers that decide whether a server build can
     * use more than one core are how often that lock is taken and how much work
     * happens per acquisition.  LOCK_ACQUIRES over POLL_CALLS is the traffic
     * ratio; POLL_SKIPPED proves the RX-pending gate is actually short-circuiting
     * readers instead of letting them poll to discover there is nothing to do.
     * BH_OVERFLOW and ALLOC_FAIL are correctness signals, not performance: a
     * non-zero value means the receive path dropped data it had accepted.
     */
    A20_PERF_NET_RX_PACKETS,
    A20_PERF_NET_RX_BYTES,
    A20_PERF_NET_TX_PACKETS,
    A20_PERF_NET_TX_BYTES,
    A20_PERF_NET_LOCK_ACQUIRES,
    A20_PERF_NET_POLL_CALLS,
    A20_PERF_NET_POLL_SKIPPED,
    A20_PERF_NET_BH_RUNS,
    A20_PERF_NET_BH_EVENTS,
    A20_PERF_NET_BH_OVERFLOW,
    A20_PERF_NET_ALLOC_FAIL,
    /* Accept-path accounting for real lwIP listening sockets.  STAGED counts
     * completed handshakes the callback parked, QUEUED counts the ones the
     * bottom half turned into a child socket, and DROP counts a handshake the
     * stack destroyed rather than delivered -- a non-zero value is a refused
     * connection, so the two must be read together to tell a healthy listener
     * from one that is dropping arrivals. */
    A20_PERF_NET_ACCEPT_STAGED,
    A20_PERF_NET_ACCEPT_QUEUED,
    A20_PERF_NET_ACCEPT_DROP,
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
void a20_perf_reset(void);

#endif /* _CORE_PERF_H */
