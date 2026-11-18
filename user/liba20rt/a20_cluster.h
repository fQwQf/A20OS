/* A20OS Native cluster syscall wrappers. The kernel provides loopback,
 * UDP/UART adapters and bounded reliable proxies; remote name resolution
 * belongs to the control plane. See docs/zh/cluster/01-abi.md for ownership,
 * handshake and channel failure semantics. */
#ifndef _A20_CLUSTER_H
#define _A20_CLUSTER_H

#include <stdint.h>
#include "a20_types.h"
#include "a20_syscall.h"
#include "a20_string.h"
#include "../../kernel/include/uapi/a20/cluster_snapshot.h"

static inline a20_status_t
a20_cluster_snapshot(a20_cluster_snapshot_args_t *out) {
    out->size = sizeof(*out);
    out->version = 1;
    return a20_syscall6(A20_SYS_cluster_snapshot, (uint64_t)out, 0, 0, 0, 0, 0);
}

static inline a20_status_t
a20_cluster_resources(a20_cluster_resource_args_t *out) {
    out->size = sizeof(*out);
    out->version = 1;
    out->kind = A20_CLUSTER_SNAPSHOT_RESOURCES;
    out->index = 0;
    out->generation = out->reserved = 0;
    return a20_syscall6(A20_SYS_cluster_snapshot, (uint64_t)out, 0, 0, 0, 0, 0);
}

static inline a20_status_t
a20_cluster_link_resources(a20_cluster_resource_args_t *out) {
    out->size = sizeof(*out);
    out->version = 1;
    out->kind = A20_CLUSTER_SNAPSHOT_LINKS;
    out->index = 0;
    out->generation = out->reserved = 0;
    return a20_syscall6(A20_SYS_cluster_snapshot, (uint64_t)(uintptr_t)out, 0,
                        0, 0, 0, 0);
}

static inline a20_status_t
a20_cluster_reassembly_resources(a20_cluster_resource_args_t *out) {
    out->size = sizeof(*out);
    out->version = 1;
    out->kind = A20_CLUSTER_SNAPSHOT_REASSEMBLY;
    out->index = 0;
    out->generation = out->reserved = 0;
    return a20_syscall6(A20_SYS_cluster_snapshot, (uint64_t)out, 0, 0, 0, 0, 0);
}

/* All-zero means this machine; addressing (LOCAL, slot) stays on the local
 * fast path and is never serialized. */
static inline void a20_node_id_local(a20_node_id_t *out) {
    int i;
    for (i = 0; i < 16; i++)
        out->bytes[i] = 0;
}

static inline int a20_node_id_is_local(const a20_node_id_t *id) {
    int i;
    for (i = 0; i < 16; i++)
        if (id->bytes[i])
            return 0;
    return 1;
}

static inline void a20_node_id_broadcast(a20_node_id_t *out) {
    int i;
    for (i = 0; i < 16; i++)
        out->bytes[i] = 0xff;
}

static inline void a20_node_id_set_raw(a20_node_id_t *out,
                                       const void *bytes16) {
    int i;
    const uint8_t *in = (const uint8_t *)bytes16;
    for (i = 0; i < 16; i++)
        out->bytes[i] = in[i];
}

static inline int a20_node_id_parse_hex(const char *text, a20_node_id_t *id) {
    if (!text || !id || a20_strlen(text) != 32)
        return -1;
    for (unsigned i = 0; i < 16; i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = text[i * 2 + j];
            unsigned digit;
            if (c >= '0' && c <= '9')
                digit = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = (unsigned)(c - 'A' + 10);
            else
                return -1;
            value = value * 16 + digit;
        }
        id->bytes[i] = (uint8_t)value;
    }
    return a20_node_id_is_local(id) ? -1 : 0;
}

/* Strict dotted IPv4:port, encoded as the six UDP next-hop bytes. */
static inline int a20_cluster_parse_udp(const char *text, uint8_t next_hop[6]) {
    if (!text || !next_hop)
        return -1;
    for (unsigned i = 0; i < 5; i++) {
        unsigned value = 0, digits = 0, limit = i == 4 ? 65535 : 255;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (unsigned)(*text++ - '0');
            if (++digits > 5 || value > limit)
                return -1;
        }
        if (!digits)
            return -1;
        if (i < 4) {
            if (*text++ != (i == 3 ? ':' : '.'))
                return -1;
            next_hop[i] = (uint8_t)value;
        } else {
            if (*text || !value)
                return -1;
            next_hop[4] = (uint8_t)(value >> 8);
            next_hop[5] = (uint8_t)value;
        }
    }
    return 0;
}

/* FNV-1a 32-bit node hash, the value that goes in a frame header
 * (docs/zh/cluster/01-abi.md §节点哈希).  Also available host-side so tests can
 * predict what a peer will put on the wire. */
static inline uint32_t a20_node_hash(const a20_node_id_t *id) {
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 16; i++) {
        h ^= id->bytes[i];
        h *= 16777619u;
    }
    return h;
}

static inline a20_status_t a20_cluster_set_self(const a20_node_id_t *node_id,
                                                uint32_t caps) {
    a20_cluster_set_self_args_t args;
    args.size = sizeof(args);
    args.version = 1;
    args.caps = caps;
    args._pad = 0;
    args.reserved[0] = 0;
    args.reserved[1] = 0;
    if (node_id)
        args.node_id = *node_id;
    else
        a20_node_id_local(&args.node_id);
    return a20_syscall6(A20_SYS_cluster_set_self, (uint64_t)&args, 0, 0, 0, 0,
                        0);
}

/* Export a local channel endpoint under a cluster-visible service name.
 * Returns the 32-bit slot, or a negative status. */
static inline a20_status_t a20_cluster_export(a20_handle_t channel,
                                              const char *name, uint32_t flags,
                                              uint32_t *out_slot) {
    a20_cluster_export_args_t args;
    args.size = sizeof(args);
    args.version = 1;
    args.flags = flags;
    args._pad = 0;
    args.channel = channel;
    args.service_name = name;
    args.name_len = name ? (uint32_t)a20_strlen(name) : 0;
    args.reserved = 0;
    a20_status_t r =
        a20_syscall6(A20_SYS_cluster_export, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_slot)
        *out_slot = (uint32_t)r;
    return r;
}

/* Obtain a proxy endpoint for a remote (or local) slot / service name. */
static inline a20_status_t a20_cluster_connect(const a20_node_id_t *node_id,
                                               uint32_t slot, const char *name,
                                               uint32_t flags,
                                               uint32_t timeout_ms,
                                               a20_handle_t *out_ep) {
    a20_cluster_connect_args_t args;
    args.size = sizeof(args);
    args.version = 1;
    args.flags = flags;
    args.slot = slot;
    args.service_name = name;
    args.name_len = name ? (uint32_t)a20_strlen(name) : 0;
    args.timeout_ms = timeout_ms;
    args.reserved = 0;
    if (node_id)
        args.node_id = *node_id;
    else
        a20_node_id_local(&args.node_id);
    a20_status_t r =
        a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_ep)
        *out_ep = (a20_handle_t)r;
    return r;
}

/* Version-2 connect atomically binds admission to the discovered boot. */
static inline a20_status_t
a20_cluster_connect_bound_flags(const a20_node_id_t *node, uint32_t slot,
                                uint64_t expected_epoch, uint32_t flags,
                                a20_handle_t *endpoint) {
    if (!node || !endpoint || !expected_epoch ||
        (flags & ~A20_CONNECT_FLAGS_ALL) || !(flags & A20_CONNECT_RELIABLE))
        return -A20_ERR_INVALID_ARGUMENT;
    a20_cluster_connect_args_v2_t args = {0};
    args.base.size = sizeof(args);
    args.base.version = 2;
    args.base.flags = flags;
    args.base.slot = slot;
    args.base.node_id = *node;
    args.expected_boot_epoch = expected_epoch;
    a20_status_t r =
        a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r >= 0)
        *endpoint = (a20_handle_t)r;
    return r;
}

static inline a20_status_t a20_cluster_connect_bound(const a20_node_id_t *node,
                                                     uint32_t slot,
                                                     uint64_t expected_epoch,
                                                     a20_handle_t *endpoint) {
    return a20_cluster_connect_bound_flags(node, slot, expected_epoch,
                                           A20_CONNECT_RELIABLE, endpoint);
}

/* next_hop format is per-transport: loopback = 4B virtual node number,
 * udp = 4B IPv4 + 2B port, uart = 2B short address. */
static inline a20_status_t
a20_cluster_route(uint32_t op, const a20_node_id_t *node_id,
                  uint32_t transport_id, const void *next_hop,
                  uint32_t next_hop_len, uint32_t metric) {
    a20_cluster_route_args_t args;
    int i;
    args.size = sizeof(args);
    args.version = 1;
    args.op = op;
    args.transport_id = transport_id;
    args.next_hop_len = next_hop_len;
    args.metric = metric;
    args.reserved = 0;
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
                                                       a20_handle_t *out_evq) {
    a20_cluster_event_subscribe_args_t args;
    args.size = sizeof(args);
    args.version = 1;
    args.mask = mask;
    args.reserved = 0;
    a20_status_t r = a20_syscall6(A20_SYS_cluster_event_subscribe,
                                  (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_evq)
        *out_evq = (a20_handle_t)r;
    return r;
}

/* node_id == NULL aggregates every link this node knows about. */
static inline a20_status_t
a20_cluster_link_status(const a20_node_id_t *node_id,
                        a20_cluster_link_status_args_t *out) {
    a20_cluster_link_status_args_t args;
    args.size = sizeof(args);
    args.version = 1;
    args.reserved = 0;
    args._pad = 0;
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
