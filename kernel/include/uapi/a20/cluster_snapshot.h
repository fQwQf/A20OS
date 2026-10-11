#ifndef A20_CLUSTER_SNAPSHOT_H
#define A20_CLUSTER_SNAPSHOT_H
#include <stdint.h>
#define A20_CLUSTER_SNAPSHOT_SELF 0u
#define A20_CLUSTER_SNAPSHOT_ROUTE 1u
#define A20_CLUSTER_SNAPSHOT_EXPORT 2u
#define A20_CLUSTER_SNAPSHOT_RESOURCES 3u
#define A20_CLUSTER_SNAPSHOT_REASSEMBLY 4u

/* Same 192-byte syscall envelope; resource samples have no enumeration
 * cursor. Counts are coherent within each subsystem, not a global stop. */
enum a20_cluster_resource_metric {
    A20_CLX_STAT_SAMPLE_MS,
    A20_CLX_STAT_BOOT_EPOCH,
    A20_CLX_STAT_ROUTES,
    A20_CLX_STAT_EXPORTS,
    A20_CLX_STAT_PROXIES,
    A20_CLX_STAT_INFLIGHT,
    A20_CLX_STAT_RX_QUEUED,
    A20_CLX_STAT_REPLAY_ENTRIES,
    A20_CLX_STAT_ORIGINS,
    A20_CLX_STAT_INFLIGHT_BYTES,
    A20_CLX_STAT_REPLAY_BYTES,
    A20_CLX_STAT_RX_BYTES,
    A20_CLX_STAT_INFLIGHT_PEAK_BYTES,
    A20_CLX_STAT_REPLAY_PEAK_BYTES,
    A20_CLX_STAT_RX_PEAK_BYTES,
    A20_CLX_STAT_MAX_ROUTES,
    A20_CLX_STAT_MAX_EXPORTS,
    A20_CLX_STAT_MAX_PROXIES,
    A20_CLX_STAT_MAX_INFLIGHT,
    A20_CLX_STAT_MAX_REPLAY_BYTES,
    A20_CLX_STAT_COUNT
};
/* kind=REASSEMBLY uses the same fixed 20-value envelope. Unused values zero. */
enum a20_cluster_reassembly_metric {
    A20_CLX_REASM_SAMPLE_MS,
    A20_CLX_REASM_CONTEXTS,
    A20_CLX_REASM_BYTES,
    A20_CLX_REASM_PEAK_BYTES,
    A20_CLX_REASM_MAX_CONTEXTS,
    A20_CLX_REASM_MAX_BYTES,
    A20_CLX_REASM_COMPLETED,
    A20_CLX_REASM_REJECTED,
    A20_CLX_REASM_EXPIRED
};
typedef struct a20_cluster_resource_args {
    uint32_t size, version, kind, index;
    uint64_t generation, reserved;
    uint64_t values[A20_CLX_STAT_COUNT];
} a20_cluster_resource_args_t;
/* Read-only native ABI. Begin with index=0, generation=0; continue using
 * next_index and the returned generation. BUSY means restart enumeration;
 * NOT_FOUND marks the end. Output fields are never input validation. */
typedef struct a20_cluster_snapshot_args {
    uint32_t size, version, kind, index;
    uint64_t generation, reserved;
    uint32_t next_index, profile;
    uint8_t node_id[16];
    uint64_t boot_epoch;
    uint32_t caps, transport_id, next_hop_len, metric;
    uint8_t next_hop[16];
    uint32_t slot, flags, name_len, max_routes, max_exports, max_remote_eps;
    uint32_t max_inflight, reserved_out;
    char name[64];
} a20_cluster_snapshot_args_t;
#endif
