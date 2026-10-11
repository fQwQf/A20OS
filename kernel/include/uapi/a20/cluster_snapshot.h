#ifndef A20_CLUSTER_SNAPSHOT_H
#define A20_CLUSTER_SNAPSHOT_H
#include <stdint.h>
#define A20_CLUSTER_SNAPSHOT_SELF 0u
#define A20_CLUSTER_SNAPSHOT_ROUTE 1u
#define A20_CLUSTER_SNAPSHOT_EXPORT 2u
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
