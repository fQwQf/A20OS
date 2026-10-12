#ifndef A20_CLUSTER_CLIENT_H
#define A20_CLUSTER_CLIENT_H
#include "a20_cluster_agent.h"
#include "../../kernel/include/uapi/a20/cluster_directory.h"
#include "../../kernel/include/uapi/a20/cluster_control.h"

/* Sequential endpoint RPC. Close the endpoint after an error: a late reply
 * must never be consumed as the next request's response. */
static inline a20_status_t
a20_cluster_exchange(a20_handle_t ep, const void *input, uint32_t len,
                     uint8_t *output, uint32_t *capacity, uint32_t budget_ms) {
    if (!budget_ms || budget_ms > 30000 || !capacity)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_status_t r =
        a20_channel_send_flags(ep, input, len, 0, 0, A20_MSG_NONBLOCK);
    if (r < 0)
        return r;
    uint64_t started = a20_agent_clock_ms(0);
    uint32_t max = *capacity;
    for (;;) {
        uint32_t handles = 0;
        *capacity = max;
        r = a20_channel_recv_flags(ep, output, capacity, 0, &handles,
                                   A20_MSG_NONBLOCK);
        if (r != -A20_ERR_WOULD_BLOCK)
            return r;
        if (a20_agent_clock_ms(0) - started >= budget_ms)
            return -A20_ERR_TIMED_OUT;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
}

/* Query an already connected, epoch-bound service. Old agents return
 * UNSUPPORTED: absence of metadata must never count as a matching image.
 * As with exchange, close the endpoint on an error before issuing more RPCs. */
static inline a20_status_t
a20_cluster_execution_query(a20_handle_t ep, uint32_t budget_ms,
                            a20_execution_info_t *out) {
    if (!out)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_work_request_t query = {.kind = A20_WORK_QUERY_EXECUTION,
                                .job = a20_agent_clock_ms(0) | 1};
    uint8_t request[A20_WORK_HEADER],
        reply[A20_WORK_HEADER + A20_EXECUTION_BYTES];
    int n = a20_work_encode(request, sizeof(request), &query);
    uint32_t length = sizeof(reply);
    a20_status_t r =
        a20_cluster_exchange(ep, request, n, reply, &length, budget_ms);
    if (r < 0)
        return r;
    a20_work_result_t result;
    if (a20_work_result_decode(reply, length, &query, &result) < 0)
        return -A20_ERR_INVALID_ARGUMENT;
    if (result.status == A20_WORK_UNSUPPORTED)
        return -A20_ERR_NOT_SUPPORTED;
    if (result.status ||
        a20_execution_decode(result.output, result.output_len, out) < 0)
        return -A20_ERR_INVALID_ARGUMENT;
    return A20_OK;
}

/* Explicit runtime discovery. UNBOUND is returned as such; old CD/1 replies
 * are NOT_SUPPORTED, never inferred to be the caller's instance. */
static inline a20_status_t
a20_cluster_runtime_query(const a20_node_id_t *node, uint32_t budget_ms,
                          a20_runtime_info_t *out, uint64_t *transport_epoch) {
    if (!node || !out || !budget_ms || budget_ms > 30000)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_handle_t ep = A20_HANDLE_NULL;
    uint64_t started = a20_agent_clock_ms(0);
    a20_status_t status;
    do {
        status = a20_cluster_connect(node, A20_DIRECTORY_SLOT, 0,
                                     A20_CONNECT_RELIABLE, 0, &ep);
        if (status != -A20_ERR_NODE_UNREACHABLE)
            break;
        if (a20_agent_clock_ms(0) - started >= budget_ms)
            return -A20_ERR_TIMED_OUT;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    } while (1);
    if (status < 0)
        return status;
    uint64_t elapsed = a20_agent_clock_ms(0) - started;
    if (elapsed >= budget_ms) {
        a20_hdl_close(ep);
        return -A20_ERR_TIMED_OUT;
    }
    uint8_t request[16], reply[A20_DIRECTORY_RUNTIME_REPLY];
    a20_directory_runtime_request(request, 0, 0);
    uint32_t length = sizeof(reply);
    status = a20_cluster_exchange(ep, request, sizeof(request), reply, &length,
                                  budget_ms - (uint32_t)elapsed);
    a20_hdl_close(ep);
    if (status < 0)
        return status;
    if (length == A20_DIRECTORY_REPLY && reply[0] == 'C' && reply[1] == 'D' &&
        reply[2] == 1)
        return -A20_ERR_NOT_SUPPORTED;
    a20_directory_record_t record;
    a20_runtime_info_t runtime;
    if (a20_directory_runtime_decode(reply, length, node->bytes, &record,
                                     &runtime) ||
        record.status == A20_DIRECTORY_INVALID)
        return -A20_ERR_INVALID_ARGUMENT;
    *out = runtime;
    if (transport_epoch)
        *transport_epoch = record.boot_epoch;
    return A20_OK;
}

/* Resolve on one node using the fixed endpoint. CHANGED restarts enumeration
 * inside the same overall budget. LOCAL names use cluster_connect directly. */
static inline a20_status_t
a20_cluster_resolve(const a20_node_id_t *node, const char *name,
                    uint32_t name_len, uint32_t budget_ms, uint32_t *slot,
                    uint64_t *boot_epoch) {
    if (!node || !name || !name_len || name_len > 64 || !slot || !budget_ms ||
        budget_ms > 30000)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_handle_t ep = A20_HANDLE_NULL;
    uint64_t started = a20_agent_clock_ms(0), generation = 0;
    a20_status_t r;
    do {
        r = a20_cluster_connect(node, A20_DIRECTORY_SLOT, 0,
                                A20_CONNECT_RELIABLE, 0, &ep);
        if (r != -A20_ERR_NODE_UNREACHABLE)
            break;
        if (a20_agent_clock_ms(0) - started >= budget_ms)
            return -A20_ERR_TIMED_OUT;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    } while (1);
    if (r < 0)
        return r;
    uint32_t index = 0;
    for (unsigned step = 0; step < 8192; step++) {
        uint64_t elapsed = a20_agent_clock_ms(0) - started;
        if (elapsed >= budget_ms) {
            r = -A20_ERR_TIMED_OUT;
            break;
        }
        uint8_t request[16], reply[128];
        a20_directory_request(request, index, generation);
        uint32_t len = sizeof(reply);
        r = a20_cluster_exchange(ep, request, sizeof(request), reply, &len,
                                 budget_ms - (uint32_t)elapsed);
        a20_directory_record_t record;
        if (r < 0)
            break;
        if (a20_directory_decode(reply, len, node->bytes, &record) < 0) {
            r = -A20_ERR_INVALID_ARGUMENT;
            break;
        }
        if (record.status == A20_DIRECTORY_CHANGED) {
            index = 0;
            generation = 0;
            continue;
        }
        if (record.status != A20_DIRECTORY_FOUND) {
            r = -A20_ERR_NOT_FOUND;
            break;
        }
        if (record.next_index <= index ||
            (generation && record.generation != generation)) {
            r = -A20_ERR_INVALID_ARGUMENT;
            break;
        }
        if (record.name_len == name_len &&
            !a20_memcmp(record.name, name, name_len)) {
            *slot = record.slot;
            if (boot_epoch)
                *boot_epoch = record.boot_epoch;
            r = 0;
            break;
        }
        index = record.next_index;
        generation = record.generation;
        r = -A20_ERR_NO_SPACE;
    }
    a20_hdl_close(ep);
    return r;
}
typedef struct a20_cluster_selection {
    a20_node_id_t node;
    uint64_t boot_epoch;
    uint32_t slot, max_input, max_output, flags, max_inflight;
} a20_cluster_selection_t;

static inline a20_status_t
a20_cluster_resolve_any(const char *name, uint32_t kind, uint32_t operator_id,
                        uint32_t input_bytes, uint32_t budget_ms,
                        a20_cluster_selection_t *out) {
    if (!name || !out || !a20_strlen(name) || a20_strlen(name) > 64)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_handle_t control = A20_HANDLE_NULL;
    a20_status_t r =
        a20_cluster_connect(0, 0, "cluster.control", 0, 0, &control);
    if (r < 0)
        return r;
    uint8_t request[96], reply[80];
    uint32_t len = sizeof(reply);
    uint64_t cookie = a20_agent_clock_ms(0) | 1;
    a20_control_request(request, A20_CONTROL_RESOLVE, kind, operator_id,
                        input_bytes, name, (uint32_t)a20_strlen(name), cookie);
    r = a20_cluster_exchange(control, request, sizeof(request), reply, &len,
                             budget_ms);
    a20_hdl_close(control);
    if (r < 0)
        return r;
    if (len != 80 || reply[0] != 'C' || reply[1] != 'M' || reply[2] != 1 ||
        reply[3] != A20_CONTROL_RESOLVE || a20_work_u64(reply + 64) != cookie)
        return -A20_ERR_INVALID_ARGUMENT;
    if (a20_work_u32(reply + 4))
        return -(a20_status_t)a20_work_u32(reply + 4);
    if (!a20_work_u64(reply + 24) || !a20_work_u32(reply + 32) ||
        a20_work_u32(reply + 36) != A20_MEMBER_ALIVE ||
        !a20_work_u32(reply + 60))
        return -A20_ERR_INVALID_ARGUMENT;
    a20_memcpy(out->node.bytes, reply + 8, 16);
    out->boot_epoch = a20_work_u64(reply + 24);
    out->slot = a20_work_u32(reply + 32);
    out->max_input = a20_work_u32(reply + 48);
    out->max_output = a20_work_u32(reply + 52);
    out->flags = a20_work_u32(reply + 56);
    out->max_inflight = a20_work_u32(reply + 60);
    return 0;
}

static inline a20_status_t
a20_cluster_connect_uri(const char *uri, uint32_t kind, uint32_t operator_id,
                        uint32_t input_bytes, uint32_t budget_ms,
                        a20_handle_t *endpoint,
                        a20_cluster_selection_t *selection) {
    if (!uri || !endpoint || a20_strncmp(uri, "service://", 10))
        return -A20_ERR_INVALID_ARGUMENT;
    uri += 10;
    a20_cluster_selection_t target = {0};
    a20_status_t r;
    if (!a20_strncmp(uri, "any/", 4)) {
        r = a20_cluster_resolve_any(uri + 4, kind, operator_id, input_bytes,
                                    budget_ms, &target);
    } else {
        char id[33];
        if (a20_strlen(uri) < 34 || uri[32] != '/')
            return -A20_ERR_INVALID_ARGUMENT;
        a20_memcpy(id, uri, 32);
        id[32] = 0;
        if (a20_node_id_parse_hex(id, &target.node) < 0)
            return -A20_ERR_INVALID_ARGUMENT;
        r = a20_cluster_resolve(&target.node, uri + 33,
                                (uint32_t)a20_strlen(uri + 33), budget_ms,
                                &target.slot, &target.boot_epoch);
    }
    if (r < 0)
        return r;
    a20_cluster_snapshot_args_t local = {0};
    int is_local = a20_cluster_snapshot(&local) >= 0 &&
                   !a20_memcmp(local.node_id, target.node.bytes, 16);
    a20_node_id_t node = target.node;
    if (is_local)
        a20_node_id_local(&node);
    r = a20_cluster_connect_bound(&node, target.slot, target.boot_epoch,
                                  endpoint);
    if (r >= 0 && selection)
        *selection = target;
    return r;
}
#endif
