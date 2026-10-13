#ifndef A20_OBJECTS_H
#define A20_OBJECTS_H
#include "a20_cluster_client.h"
#include "../../kernel/include/uapi/a20/cluster_objects.h"
typedef struct a20_object_reference {
    a20_handle_t endpoint;
    a20_object_id_t id;
    uint64_t publication, cookie;
} a20_object_reference_t;
typedef a20_object_reference_t a20_object_publication_t;
typedef struct a20_object_snapshot {
    a20_object_id_t id;
    uint64_t publication;
    uint32_t state, remaining_ms;
} a20_object_snapshot_t;
static inline void a20_object_close(a20_object_reference_t *ref) {
    if (ref && ref->endpoint != A20_HANDLE_NULL) {
        a20_hdl_close(ref->endpoint);
        ref->endpoint = A20_HANDLE_NULL;
    }
}
static inline a20_status_t a20_object_status(uint32_t status) {
    const uint32_t errors[] = {0,
                               A20_ERR_INVALID_ARGUMENT,
                               A20_ERR_NOT_FOUND,
                               A20_ERR_EXPIRED,
                               A20_ERR_ACCESS,
                               A20_ERR_NO_SPACE,
                               A20_ERR_EXISTS,
                               A20_ERR_EXPIRED,
                               A20_ERR_IO};
    return status <= A20_OBJECT_FAILED ? -(a20_status_t)errors[status]
                                       : -A20_ERR_PROTOCOL;
}
/* Endpoint is single-flight. An error closes it, fencing delayed replies. */
static inline a20_status_t a20_object_rpc(a20_object_reference_t *ref,
                                          uint32_t op, uint32_t lease,
                                          a20_handle_t attached,
                                          uint32_t budget,
                                          a20_object_snapshot_t *out) {
    if (!ref || ref->endpoint == A20_HANDLE_NULL || !budget || budget > 30000 ||
        ref->cookie == UINT64_MAX)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_object_message_t q = {
        .op = op,
        .value = lease,
        .cookie = ++ref->cookie,
        .publication = op == A20_OBJECT_PUBLISH ? 0 : ref->publication,
        .id = ref->id};
    uint8_t request[96], reply[96];
    if (a20_object_request_encode(request, 96, &q))
        return -A20_ERR_INVALID_ARGUMENT;
    a20_status_t status = a20_channel_send_flags(
        ref->endpoint, request, 96, attached == A20_HANDLE_NULL ? 0 : &attached,
        attached == A20_HANDLE_NULL ? 0 : 1, A20_MSG_NONBLOCK);
    uint64_t started = a20_agent_clock_ms(0);
    uint32_t n = 96;
    while (status >= 0) {
        uint32_t handles = 0;
        n = 96;
        status = a20_channel_recv_flags(ref->endpoint, reply, &n, 0, &handles,
                                        A20_MSG_NONBLOCK);
        if (status != -A20_ERR_WOULD_BLOCK)
            break;
        if (a20_agent_clock_ms(0) - started >= budget) {
            status = -A20_ERR_TIMED_OUT;
            break;
        }
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
        status = 0;
    }
    a20_object_message_t response;
    if (status >= 0) {
        if (a20_object_reply_decode(reply, n, &q, &response))
            status = -A20_ERR_PROTOCOL;
        else
            status = a20_object_status(response.value);
    }
    if (status < 0) {
        a20_object_close(ref);
        return status;
    }
    if (op != A20_OBJECT_REVOKE) {
        ref->publication = response.publication;
        if (out)
            *out = (a20_object_snapshot_t){.id = ref->id,
                                           .publication = response.publication,
                                           .state = response.state,
                                           .remaining_ms =
                                               a20_work_u32(reply + 24)};
    }
    return 0;
}
static inline a20_status_t a20_object_publish(a20_handle_t task, uint32_t lease,
                                              uint32_t budget,
                                              a20_object_publication_t *out) {
    if (!out || !lease || lease > A20_OBJECT_MAX_LEASE)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_handle_identity_t identity;
    a20_status_t status = a20_hdl_identity(task, &identity);
    if (status < 0)
        return status;
    a20_object_reference_t ref = {.endpoint = A20_HANDLE_NULL,
                                  .id = identity.id};
    status = a20_cluster_connect(0, 0, A20_OBJECT_SERVICE, 0, 0, &ref.endpoint);
    if (status < 0)
        return status;
    status = a20_object_rpc(&ref, A20_OBJECT_PUBLISH, lease, task, budget, 0);
    if (status < 0) {
        a20_object_close(&ref);
        return status;
    }
    *out = ref;
    return 0;
}
static inline a20_status_t a20_object_renew(a20_object_publication_t *pub,
                                            uint32_t lease, uint32_t budget) {
    return a20_object_rpc(pub, A20_OBJECT_RENEW, lease, A20_HANDLE_NULL, budget,
                          0);
}
static inline a20_status_t a20_object_revoke(a20_object_publication_t *pub,
                                             uint32_t budget) {
    a20_status_t status =
        a20_object_rpc(pub, A20_OBJECT_REVOKE, 0, A20_HANDLE_NULL, budget, 0);
    a20_object_close(pub);
    return status;
}
static inline a20_status_t a20_object_query(a20_object_reference_t *ref,
                                            uint32_t budget,
                                            a20_object_snapshot_t *out) {
    if (!out)
        return -A20_ERR_INVALID_ARGUMENT;
    return a20_object_rpc(ref, A20_OBJECT_QUERY, 0, A20_HANDLE_NULL, budget,
                          out);
}
static inline a20_status_t a20_object_open(const a20_object_id_t *id,
                                           uint32_t budget,
                                           a20_object_reference_t *out,
                                           a20_object_snapshot_t *snapshot) {
    if (!out || !a20_object_id_valid(id) || !budget || budget > 30000)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_runtime_info_t local;
    a20_status_t status = a20_runtime_info(&local);
    if (status < 0)
        return status;
    if (local.state != A20_RUNTIME_BOUND)
        return -A20_ERR_WOULD_BLOCK;
    if (!a20_runtime_id_equal(local.instance_id, id->instance_id))
        return -A20_ERR_ACCESS;
    a20_object_reference_t ref = {.endpoint = A20_HANDLE_NULL, .id = *id};
    uint64_t started = a20_agent_clock_ms(0);
    if (a20_runtime_id_equal(local.domain_id, id->domain_id)) {
        if (local.boot_epoch != id->boot_epoch)
            return -A20_ERR_EXPIRED;
        status =
            a20_cluster_connect(0, 0, A20_OBJECT_SERVICE, 0, 0, &ref.endpoint);
    } else {
        a20_node_id_t node;
        a20_memcpy(node.bytes, id->domain_id, 16);
        a20_runtime_info_t peer;
        uint64_t transport, epoch;
        uint32_t slot;
        status = a20_cluster_runtime_query(&node, budget, &peer, &transport);
        if (status < 0)
            return status;
        if (!a20_runtime_admits(&local, &peer, node.bytes) ||
            peer.boot_epoch != id->boot_epoch)
            return -A20_ERR_EXPIRED;
        uint64_t elapsed = a20_agent_clock_ms(0) - started;
        if (elapsed >= budget)
            return -A20_ERR_TIMED_OUT;
        status = a20_cluster_resolve(&node, A20_OBJECT_SERVICE, 16,
                                     budget - (uint32_t)elapsed, &slot, &epoch);
        if (status < 0)
            return status;
        if (epoch != transport)
            return -A20_ERR_EXPIRED;
        status =
            a20_cluster_connect_bound(&node, slot, transport, &ref.endpoint);
    }
    if (status < 0)
        return status;
    uint64_t elapsed = a20_agent_clock_ms(0) - started;
    if (elapsed >= budget) {
        a20_object_close(&ref);
        return -A20_ERR_TIMED_OUT;
    }
    status =
        a20_object_query(&ref, budget - (uint32_t)elapsed,
                         snapshot ? snapshot : &(a20_object_snapshot_t){0});
    if (status < 0) {
        a20_object_close(&ref);
        return status;
    }
    *out = ref;
    return 0;
}
#endif
