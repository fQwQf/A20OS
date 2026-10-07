/*
 * A20OS Native SDK — Cluster calls (0x0520).
 *
 * Thin wrappers over the frozen cluster ABI (docs/cluster/01-abi.md).  Until
 * the kernel data plane lands, every one of these returns
 * A20_ERR_CLUSTER_UNSUPPORTED; the wrappers exist now so clusterd, the demo
 * programs and the MCU-side tooling can be written against a fixed surface.
 *
 * The interesting one is a20_cluster_connect(): on success it yields an
 * ordinary A20_OBJ_CHANNEL_ENDPOINT handle, so callers then use plain
 * a20_channel_call / a20_channel_send and never learn whether the peer is in
 * this kernel or another machine.
 */
#ifndef _A20_CLUSTER_H
#define _A20_CLUSTER_H

#include <stdint.h>
#include "a20_types.h"
#include "a20_syscall.h"
#include "a20_string.h"

/* All-zero means this machine; addressing (LOCAL, slot) stays on the local
 * fast path and is never serialized. */
static inline void a20_node_id_local(a20_node_id_t *out)
{
    int i;
    for (i = 0; i < 16; i++)
        out->bytes[i] = 0;
}

static inline int a20_node_id_is_local(const a20_node_id_t *id)
{
    int i;
    for (i = 0; i < 16; i++)
        if (id->bytes[i]) return 0;
    return 1;
}

static inline void a20_node_id_broadcast(a20_node_id_t *out)
{
    int i;
    for (i = 0; i < 16; i++)
        out->bytes[i] = 0xff;
}

static inline void a20_node_id_set_raw(a20_node_id_t *out, const void *bytes16)
{
    int i;
    const uint8_t *in = (const uint8_t *)bytes16;
    for (i = 0; i < 16; i++)
        out->bytes[i] = in[i];
}

/* FNV-1a 32-bit node hash, the value that goes in a frame header
 * (docs/cluster/01-abi.md §节点哈希).  Also available host-side so tests can
 * predict what a peer will put on the wire. */
static inline uint32_t a20_node_hash(const a20_node_id_t *id)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 16; i++) {
        h ^= id->bytes[i];
        h *= 16777619u;
    }
    return h;
}

static inline a20_status_t a20_cluster_set_self(const a20_node_id_t *node_id,
                                               uint32_t caps)
{
    a20_cluster_set_self_args_t args;
    args.size    = sizeof(args);
    args.version = 1;
    args.caps    = caps;
    args._pad    = 0;
    args.reserved[0] = 0;
    args.reserved[1] = 0;
    if (node_id)
        args.node_id = *node_id;
    else
        a20_node_id_local(&args.node_id);
    return a20_syscall6(A20_SYS_cluster_set_self, (uint64_t)&args,
                        0, 0, 0, 0, 0);
}

/* Export a local channel endpoint under a cluster-visible service name.
 * Returns the 32-bit slot, or a negative status. */
static inline a20_status_t a20_cluster_export(a20_handle_t channel,
                                             const char *name, uint32_t flags,
                                             uint32_t *out_slot)
{
    a20_cluster_export_args_t args;
    args.size         = sizeof(args);
    args.version      = 1;
    args.flags        = flags;
    args._pad         = 0;
    args.channel      = channel;
    args.service_name = name;
    args.name_len     = name ? (uint32_t)a20_strlen(name) : 0;
    args.reserved     = 0;
    a20_status_t r = a20_syscall6(A20_SYS_cluster_export, (uint64_t)&args,
                                  0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_slot)
        *out_slot = (uint32_t)r;
    return r;
}

/* Obtain a proxy endpoint for a remote (or local) slot / service name. */
static inline a20_status_t a20_cluster_connect(const a20_node_id_t *node_id,
                                               uint32_t slot,
                                               const char *name,
                                               uint32_t flags,
                                               uint32_t timeout_ms,
                                               a20_handle_t *out_ep)
{
    a20_cluster_connect_args_t args;
    args.size         = sizeof(args);
    args.version      = 1;
    args.flags        = flags;
    args.slot         = slot;
    args.service_name = name;
    args.name_len     = name ? (uint32_t)a20_strlen(name) : 0;
    args.timeout_ms   = timeout_ms;
    args.reserved     = 0;
    if (node_id)
        args.node_id = *node_id;
    else
        a20_node_id_local(&args.node_id);
    a20_status_t r = a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args,
                                  0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_ep)
        *out_ep = (a20_handle_t)r;
    return r;
}

/* next_hop format is per-transport: loopback = 4B virtual node number,
 * udp = 4B IPv4 + 2B port, uart = 2B short address. */
static inline a20_status_t a20_cluster_route(uint32_t op,
                                             const a20_node_id_t *node_id,
                                             uint32_t transport_id,
                                             const void *next_hop,
                                             uint32_t next_hop_len,
                                             uint32_t metric)
{
    a20_cluster_route_args_t args;
    int i;
    args.size         = sizeof(args);
    args.version      = 1;
    args.op           = op;
    args.transport_id = transport_id;
    args.next_hop_len = next_hop_len;
    args.metric       = metric;
    args.reserved     = 0;
    if (node_id)
        args.node_id = *node_id;
    else
        a20_node_id_local(&args.node_id);
    for (i = 0; i < 16; i++)
        args.next_hop[i] = 0;
    if (next_hop && next_hop_len) {
        const uint8_t *in = (const uint8_t *)next_hop;
        for (i = 0; i < (int)next_hop_len && i < 16; i++)
            args.next_hop[i] = in[i];
    }
    return a20_syscall6(A20_SYS_cluster_route, (uint64_t)&args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_cluster_event_subscribe(uint32_t mask,
                                                       a20_handle_t *out_evq)
{
    a20_cluster_event_subscribe_args_t args;
    args.size     = sizeof(args);
    args.version  = 1;
    args.mask     = mask;
    args.reserved = 0;
    a20_status_t r =
        a20_syscall6(A20_SYS_cluster_event_subscribe, (uint64_t)&args,
                     0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_evq)
        *out_evq = (a20_handle_t)r;
    return r;
}

/* node_id == NULL aggregates every link this node knows about. */
static inline a20_status_t a20_cluster_link_status(
    const a20_node_id_t *node_id, a20_cluster_link_status_args_t *out)
{
    a20_cluster_link_status_args_t args;
    args.size     = sizeof(args);
    args.version  = 1;
    args.reserved = 0;
    args._pad     = 0;
    if (node_id)
        args.node_id = *node_id;
    else
        a20_node_id_local(&args.node_id);
    a20_status_t r = a20_syscall6(A20_SYS_cluster_link_status, (uint64_t)&args,
                                  0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out)
        *out = args;
    return r;
}

#endif /* _A20_CLUSTER_H */