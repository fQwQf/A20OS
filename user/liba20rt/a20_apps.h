#ifndef A20_APPS_H
#define A20_APPS_H
#include "a20_cluster_client.h"
#include "../../kernel/include/uapi/a20/cluster_apps.h"
a20_start_info_t *a20_get_start_info(void);
typedef struct a20_app_session {
    a20_handle_t endpoint;
    a20_object_id_t controller, client;
    a20_execution_info_t execution;
    uint64_t cookie;
} a20_app_session_t;
typedef struct a20_app_ticket {
    a20_object_id_t controller, client;
    uint8_t manifest[16];
    uint64_t nonce;
} a20_app_ticket_t;
static inline void a20_app_close(a20_app_session_t *s) {
    if (s && s->endpoint != A20_HANDLE_NULL) {
        a20_hdl_close(s->endpoint);
        s->endpoint = A20_HANDLE_NULL;
    }
}
static inline a20_status_t a20_app_exchange(a20_app_session_t *s,
                                            a20_app_request_t *q,
                                            uint32_t budget,
                                            a20_app_result_t *out) {
    if (!s || s->endpoint == A20_HANDLE_NULL || !out || !budget ||
        budget > 30000 || s->cookie == UINT64_MAX)
        return -A20_ERR_INVALID_ARGUMENT;
    q->cookie = ++s->cookie;
    uint8_t request[A20_APP_REQUEST], reply[A20_APP_REPLY];
    if (a20_app_request_encode(request, sizeof(request), q))
        return -A20_ERR_INVALID_ARGUMENT;
    uint32_t len = sizeof(reply);
    a20_status_t status = a20_cluster_exchange(
        s->endpoint, request, sizeof(request), reply, &len, budget);
    a20_app_result_t result;
    if (status >= 0) {
        if (a20_app_result_decode(reply, len, q, &result))
            status = -A20_ERR_PROTOCOL;
        else if (result.status) {
            static const unsigned errors[] = {0,
                                              A20_ERR_INVALID_ARGUMENT,
                                              A20_ERR_EXPIRED,
                                              A20_ERR_NOT_FOUND,
                                              A20_ERR_EXISTS,
                                              A20_ERR_NO_SPACE,
                                              A20_ERR_NOT_SUPPORTED,
                                              A20_ERR_IO,
                                              A20_ERR_WOULD_BLOCK};
            status = -(a20_status_t)errors[result.status];
        }
    }
    if (status < 0) {
        a20_app_close(s);
        return status;
    }
    *out = result;
    return 0;
}
/* Explicit owner; the complete lookup/hello shares one budget. No implicit
 * retry/failover of a request which may already have started an application. */
static inline a20_status_t a20_app_connect(const uint8_t domain[16],
                                           uint32_t budget,
                                           a20_app_session_t *out) {
    if (!out || !a20_runtime_id_valid(domain) || !budget || budget > 30000)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_runtime_info_t local, peer;
    a20_start_info_t *si = a20_get_start_info();
    a20_handle_identity_t self;
    a20_status_t status = a20_runtime_info(&local);
    if (status < 0)
        return status;
    if (local.state != A20_RUNTIME_BOUND)
        return -A20_ERR_WOULD_BLOCK;
    if (!si || a20_hdl_identity(si->self_task, &self) < 0)
        return -A20_ERR_BAD_HANDLE;
    a20_app_session_t s = {.endpoint = A20_HANDLE_NULL, .client = self.id};
    uint64_t start = a20_agent_clock_ms(0);
    if (a20_runtime_id_equal(domain, local.domain_id)) {
        peer = local;
        status = a20_cluster_connect(0, 0, A20_APP_SERVICE, 0, 0, &s.endpoint);
    } else {
        a20_node_id_t node;
        a20_memcpy(node.bytes, domain, 16);
        uint64_t transport, epoch;
        uint32_t slot;
        status = a20_cluster_runtime_query(&node, budget, &peer, &transport);
        if (status < 0)
            return status;
        if (!a20_runtime_admits(&local, &peer, domain))
            return -A20_ERR_ACCESS;
        uint64_t elapsed = a20_agent_clock_ms(0) - start;
        if (elapsed >= budget)
            return -A20_ERR_TIMED_OUT;
        status = a20_cluster_resolve(&node, A20_APP_SERVICE, 13,
                                     budget - (uint32_t)elapsed, &slot, &epoch);
        if (status < 0)
            return status;
        if (epoch != transport)
            return -A20_ERR_EXPIRED;
        status = a20_cluster_connect_bound(&node, slot, transport, &s.endpoint);
    }
    if (status < 0)
        return status;
    uint64_t elapsed = a20_agent_clock_ms(0) - start;
    if (elapsed >= budget) {
        a20_app_close(&s);
        return -A20_ERR_TIMED_OUT;
    }
    a20_app_request_t q = {.op = A20_APP_HELLO};
    a20_app_result_t hello;
    status = a20_app_exchange(&s, &q, budget - (uint32_t)elapsed, &hello);
    if (status < 0)
        return status;
    if (!a20_runtime_id_equal(hello.controller.instance_id, peer.instance_id) ||
        !a20_runtime_id_equal(hello.controller.domain_id, peer.domain_id) ||
        hello.controller.boot_epoch != peer.boot_epoch) {
        a20_app_close(&s);
        return -A20_ERR_EXPIRED;
    }
    s.controller = hello.controller;
    s.execution = hello.execution;
    *out = s;
    return 0;
}
static inline a20_status_t a20_app_resume(const a20_app_ticket_t *ticket,
                                          uint32_t budget,
                                          a20_app_session_t *out) {
    if (!ticket || !a20_object_id_valid(&ticket->controller) ||
        !a20_object_id_valid(&ticket->client) ||
        !a20_runtime_id_valid(ticket->manifest) || !ticket->nonce || !out)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_app_session_t s;
    a20_status_t status =
        a20_app_connect(ticket->controller.domain_id, budget, &s);
    if (status < 0)
        return status;
    if (!a20_object_id_equal(&ticket->controller, &s.controller) ||
        !a20_object_id_equal(&ticket->client, &s.client)) {
        a20_app_close(&s);
        return -A20_ERR_EXPIRED;
    }
    *out = s;
    return 0;
}
static inline a20_status_t
a20_app_call(a20_app_session_t *s, const a20_app_ticket_t *ticket, unsigned op,
             unsigned reason, uint32_t budget, a20_app_result_t *out) {
    if (!s || !ticket ||
        !a20_object_id_equal(&s->controller, &ticket->controller) ||
        !a20_object_id_equal(&s->client, &ticket->client))
        return -A20_ERR_EXPIRED;
    a20_app_request_t q = {.op = op,
                           .reason = reason,
                           .nonce = ticket->nonce,
                           .controller = ticket->controller,
                           .client = ticket->client};
    a20_memcpy(q.manifest, ticket->manifest, 16);
    return a20_app_exchange(s, &q, budget, out);
}
static inline a20_status_t a20_app_start(a20_app_session_t *s,
                                         const uint8_t manifest[16],
                                         uint64_t nonce, uint32_t budget,
                                         a20_app_ticket_t *ticket,
                                         a20_app_result_t *out) {
    if (!s || !ticket || !out || !a20_runtime_id_valid(manifest) || !nonce)
        return -A20_ERR_INVALID_ARGUMENT;
    /* Return the ticket before transport: a timeout is an unknown outcome,
     * and the caller needs this exact key to query or explicitly retry. */
    *ticket = (a20_app_ticket_t){
        .controller = s->controller, .client = s->client, .nonce = nonce};
    a20_memcpy(ticket->manifest, manifest, 16);
    return a20_app_call(s, ticket, A20_APP_START, 0, budget, out);
}
static inline a20_status_t a20_app_query(a20_app_session_t *s,
                                         const a20_app_ticket_t *ticket,
                                         uint32_t budget,
                                         a20_app_result_t *out) {
    return a20_app_call(s, ticket, A20_APP_QUERY, 0, budget, out);
}
static inline a20_status_t a20_app_stop(a20_app_session_t *s,
                                        const a20_app_ticket_t *ticket,
                                        unsigned reason, uint32_t budget,
                                        a20_app_result_t *out) {
    return a20_app_call(s, ticket, A20_APP_STOP, reason, budget, out);
}
/* Query a compatible manifest and real outstanding application count. */
static inline a20_status_t a20_app_offer(a20_app_session_t *s,
                                         const uint8_t manifest[16],
                                         uint32_t budget,
                                         a20_app_result_t *out) {
    if (!a20_runtime_id_valid(manifest))
        return -A20_ERR_INVALID_ARGUMENT;
    a20_app_request_t q = {.op = A20_APP_HELLO};
    a20_memcpy(q.manifest, manifest, 16);
    a20_status_t status = a20_app_exchange(s, &q, budget, out);
    if (status >= 0 && !a20_object_id_equal(&s->controller, &out->controller)) {
        a20_app_close(s);
        return -A20_ERR_EXPIRED;
    }
    return status;
}
/* Functional placement: select the compatible owner with least actual live
 * applications. Equal owners use input order. The owner atomically checks the
 * sampled count before accepting START. No second owner after an unknown
 * outcome; the returned ticket is the sole query/retry destination. */
static inline a20_status_t
a20_app_start_any(const uint8_t (*domains)[16], unsigned count,
                  const uint8_t manifest[16], uint64_t nonce, uint32_t budget,
                  a20_app_ticket_t *ticket, a20_app_result_t *out) {
    if (!domains || !count || count > 16 || !a20_runtime_id_valid(manifest) ||
        !nonce || !budget || budget > 30000 || !ticket || !out)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_app_session_t best = {.endpoint = A20_HANDLE_NULL};
    uint32_t load = UINT32_MAX;
    uint64_t start = a20_agent_clock_ms(0);
    for (unsigned i = 0; i < count; i++) {
        uint64_t elapsed = a20_agent_clock_ms(0) - start;
        if (elapsed >= budget)
            break;
        uint32_t slice = (budget - (uint32_t)elapsed) / (count - i);
        if (!slice)
            break;
        uint64_t attempt = a20_agent_clock_ms(0);
        a20_app_session_t s;
        a20_app_result_t offer;
        if (a20_app_connect(domains[i], slice, &s) < 0)
            continue;
        elapsed = a20_agent_clock_ms(0) - attempt;
        if (elapsed >= slice) {
            a20_app_close(&s);
            continue;
        }
        if (a20_app_offer(&s, manifest, slice - (uint32_t)elapsed, &offer) < 0)
            continue;
        if (offer.occupied < offer.capacity && offer.active < load) {
            a20_app_close(&best);
            best = s;
            load = offer.active;
        } else
            a20_app_close(&s);
    }
    if (best.endpoint == A20_HANDLE_NULL)
        return -A20_ERR_NOT_FOUND;
    uint64_t elapsed = a20_agent_clock_ms(0) - start;
    if (elapsed >= budget) {
        a20_app_close(&best);
        return -A20_ERR_TIMED_OUT;
    }
    *ticket = (a20_app_ticket_t){
        .controller = best.controller, .client = best.client, .nonce = nonce};
    a20_memcpy(ticket->manifest, manifest, 16);
    a20_status_t status = a20_app_call(&best, ticket, A20_APP_START, load + 1,
                                       budget - (uint32_t)elapsed, out);
    a20_app_close(&best);
    return status;
}
#endif
