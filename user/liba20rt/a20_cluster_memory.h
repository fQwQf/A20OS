#ifndef A20_CLUSTER_MEMORY_CLIENT_H
#define A20_CLUSTER_MEMORY_CLIENT_H
#include "a20_cluster_client.h"
#include "../../kernel/include/uapi/a20/cluster_memory.h"
a20_start_info_t *a20_get_start_info(void);

typedef struct a20_memory_session {
    a20_handle_t endpoint;
    uint8_t owner[64], client[64];
    uint64_t cookie;
    a20_memory_config_t config;
    uint32_t permission;
} a20_memory_session_t;
typedef struct a20_memory_ticket {
    uint8_t owner[64], client[64];
    uint64_t version, token, nonce;
    uint32_t page;
} a20_memory_ticket_t;

static inline void a20_memory_close(a20_memory_session_t *s) {
    if (s && s->endpoint != A20_HANDLE_NULL) {
        a20_hdl_close(s->endpoint);
        s->endpoint = A20_HANDLE_NULL;
    }
}
static inline a20_status_t a20_memory_error(uint32_t status) {
    static const unsigned errors[] = {0,
                                      A20_ERR_INVALID_ARGUMENT,
                                      A20_ERR_NOT_SUPPORTED,
                                      A20_ERR_ACCESS,
                                      A20_ERR_EXPIRED,
                                      A20_ERR_EXISTS,
                                      A20_ERR_WOULD_BLOCK,
                                      A20_ERR_WOULD_BLOCK,
                                      A20_ERR_WOULD_BLOCK,
                                      A20_ERR_NO_SPACE};
    return status < sizeof(errors) / sizeof(errors[0])
               ? -(a20_status_t)errors[status]
               : -A20_ERR_PROTOCOL;
}
static inline a20_status_t a20_memory_rpc(a20_memory_session_t *s, uint8_t *q,
                                          uint32_t len,
                                          uint8_t reply[A20_MEMORY_MESSAGE],
                                          uint32_t *reply_len,
                                          uint32_t budget_ms) {
    if (!s || s->endpoint == A20_HANDLE_NULL || !q || !reply || !reply_len ||
        !budget_ms || budget_ms > 30000 || s->cookie == UINT64_MAX ||
        s->permission == A20_MEMORY_OFF)
        return -A20_ERR_INVALID_ARGUMENT;
    if (q[3] >= A20_MEMORY_BEGIN && (s->permission != A20_MEMORY_READ_WRITE ||
                                     s->config.mode != A20_MEMORY_READ_WRITE))
        return -A20_ERR_ACCESS;
    a20_work_put64(q + 8, ++s->cookie);
    *reply_len = A20_MEMORY_MESSAGE;
    a20_status_t status =
        a20_cluster_exchange(s->endpoint, q, len, reply, reply_len, budget_ms);
    if (status < 0) {
        a20_memory_close(s);
        return status;
    }
    if (*reply_len < A20_MEMORY_HEADER) {
        a20_memory_close(s);
        return -A20_ERR_PROTOCOL;
    }
    uint32_t expected = A20_MEMORY_HEADER;
    if (q[3] == A20_MEMORY_READ && !a20_work_u32(reply + 4))
        expected += a20_work_u32(q + 24);
    if (*reply_len != expected || a20_memcmp(q, reply, 4) ||
        a20_work_u64(reply + 8) != s->cookie || a20_work_u64(reply + 56) ||
        a20_memcmp(reply + 128, s->client, 64) ||
        a20_work_u32(reply + 4) > A20_MEMORY_EXHAUSTED) {
        a20_memory_close(s);
        return -A20_ERR_PROTOCOL;
    }
    if (q[3] != A20_MEMORY_HELLO && a20_memcmp(reply + 64, s->owner, 64)) {
        a20_memory_close(s);
        return -A20_ERR_EXPIRED;
    }
    if (q[3] != A20_MEMORY_HELLO &&
        (a20_work_u32(reply + 16) != a20_work_u32(q + 16) ||
         a20_work_u32(reply + 20) != a20_work_u32(q + 20) ||
         a20_work_u32(reply + 24) != a20_work_u32(q + 24) ||
         a20_work_u32(reply + 28) ||
         a20_work_u64(reply + 48) != a20_work_u64(q + 48))) {
        a20_memory_close(s);
        return -A20_ERR_PROTOCOL;
    }
    return a20_memory_error(a20_work_u32(reply + 4));
}
static inline uint32_t a20_memory_remaining(uint64_t start, uint32_t budget) {
    uint64_t elapsed = a20_agent_clock_ms(0) - start;
    return elapsed < budget ? budget - (uint32_t)elapsed : 0;
}

/* Permission OFF returns before discovery, network or allocation. Explicit
 * remote owner only: never fail over after a possibly accepted operation. */
static inline a20_status_t a20_memory_connect(const uint8_t domain[16],
                                              uint32_t permission,
                                              uint32_t budget_ms,
                                              a20_memory_session_t *out) {
    if (!out || !a20_runtime_id_valid(domain) ||
        permission > A20_MEMORY_READ_WRITE || !budget_ms || budget_ms > 30000)
        return -A20_ERR_INVALID_ARGUMENT;
    if (permission == A20_MEMORY_OFF)
        return -A20_ERR_NOT_SUPPORTED;
    a20_memory_session_t s = {.endpoint = A20_HANDLE_NULL,
                              .permission = permission};
    a20_runtime_info_t local, peer;
    a20_handle_identity_t self;
    a20_start_info_t *si = a20_get_start_info();
    a20_status_t status = a20_runtime_info(&local);
    if (status < 0)
        return status;
    if (local.state != A20_RUNTIME_BOUND || !si ||
        a20_hdl_identity(si->self_task, &self) < 0 ||
        a20_object_id_encode(s.client, 64, &self.id))
        return -A20_ERR_ACCESS;
    uint64_t start = a20_agent_clock_ms(0);
    if (a20_runtime_id_equal(domain, local.domain_id)) {
        peer = local;
        status =
            a20_cluster_connect(0, 0, A20_MEMORY_SERVICE, 0, 0, &s.endpoint);
    } else {
        a20_node_id_t node;
        a20_memcpy(node.bytes, domain, 16);
        uint64_t transport, epoch;
        uint32_t slot;
        status = a20_cluster_runtime_query(&node, budget_ms, &peer, &transport);
        if (status < 0)
            return status;
        if (!a20_runtime_admits(&local, &peer, domain))
            return -A20_ERR_ACCESS;
        uint32_t left = a20_memory_remaining(start, budget_ms);
        if (!left)
            return -A20_ERR_TIMED_OUT;
        status = a20_cluster_resolve(&node, A20_MEMORY_SERVICE, 14, left, &slot,
                                     &epoch);
        if (status < 0)
            return status;
        if (epoch != transport)
            return -A20_ERR_EXPIRED;
        status = a20_cluster_connect_bound(&node, slot, transport, &s.endpoint);
    }
    if (status < 0)
        return status;
    uint8_t q[A20_MEMORY_HEADER], r[A20_MEMORY_MESSAGE];
    a20_memory_request(q, A20_MEMORY_HELLO, 1, 0, s.client);
    uint32_t len, left = a20_memory_remaining(start, budget_ms);
    status = left ? a20_memory_rpc(&s, q, sizeof(q), r, &len, left)
                  : -A20_ERR_TIMED_OUT;
    if (status < 0) {
        a20_memory_close(&s);
        return status;
    }
    a20_object_id_t identity;
    if (a20_object_id_decode(r + 64, 64, &identity) ||
        !a20_runtime_id_equal(identity.instance_id, peer.instance_id) ||
        !a20_runtime_id_equal(identity.domain_id, peer.domain_id) ||
        identity.boot_epoch != peer.boot_epoch) {
        a20_memory_close(&s);
        return -A20_ERR_EXPIRED;
    }
    s.config =
        (a20_memory_config_t){a20_work_u32(r + 16), a20_work_u32(r + 20),
                              a20_work_u32(r + 24), a20_work_u32(r + 28)};
    if (!s.config.mode || s.config.mode > A20_MEMORY_READ_WRITE ||
        !s.config.pages || s.config.pages > 64 || !s.config.transaction_ms ||
        s.config.transaction_ms > 30000 ||
        (s.config.bytes_per_second &&
         s.config.bytes_per_second < A20_MEMORY_CHUNK)) {
        a20_memory_close(&s);
        return -A20_ERR_PROTOCOL;
    }
    a20_memcpy(s.owner, r + 64, 64);
    *out = s;
    return 0;
}

/* Returns one version-consistent PRIVATE snapshot. On conflict/error, neither
 * caller buffer nor version changes. No cached pointer is ever auto-refreshed. */
static inline a20_status_t a20_memory_read_page(a20_memory_session_t *s,
                                                uint32_t page, void *buffer,
                                                uint64_t *version,
                                                uint32_t budget_ms) {
    if (!s || !buffer || !version || page >= s->config.pages || !budget_ms ||
        budget_ms > 30000)
        return -A20_ERR_INVALID_ARGUMENT;
    uint8_t snapshot[A20_MEMORY_PAGE], q[A20_MEMORY_HEADER],
        r[A20_MEMORY_MESSAGE];
    uint64_t start = a20_agent_clock_ms(0), observed = 0;
    for (uint32_t offset = 0; offset < A20_MEMORY_PAGE;
         offset += A20_MEMORY_CHUNK) {
        uint32_t left = a20_memory_remaining(start, budget_ms), len;
        if (!left)
            return -A20_ERR_TIMED_OUT;
        a20_memory_request(q, A20_MEMORY_READ, 1, s->owner, s->client);
        a20_work_put32(q + 16, page);
        a20_work_put32(q + 20, offset);
        a20_work_put32(q + 24, A20_MEMORY_CHUNK);
        a20_work_put64(q + 32, observed);
        a20_status_t status = a20_memory_rpc(s, q, sizeof(q), r, &len, left);
        if (status < 0)
            return status;
        uint64_t next = a20_work_u64(r + 32);
        if (!next || (observed && observed != next)) {
            a20_memory_close(s);
            return -A20_ERR_PROTOCOL;
        }
        observed = next;
        a20_memcpy(snapshot + offset, r + A20_MEMORY_HEADER, A20_MEMORY_CHUNK);
    }
    a20_memcpy(buffer, snapshot, sizeof(snapshot));
    *version = observed;
    return 0;
}
static inline a20_status_t
a20_memory_begin(a20_memory_session_t *s, uint32_t page, uint64_t version,
                 uint64_t nonce, a20_memory_ticket_t *out, uint32_t budget_ms) {
    if (!s || !out || page >= s->config.pages || !version || !nonce)
        return -A20_ERR_INVALID_ARGUMENT;
    uint8_t q[A20_MEMORY_HEADER], r[A20_MEMORY_MESSAGE];
    a20_memory_request(q, A20_MEMORY_BEGIN, 1, s->owner, s->client);
    a20_work_put32(q + 16, page);
    a20_work_put64(q + 32, version);
    a20_work_put64(q + 48, nonce);
    uint32_t len;
    a20_status_t status = a20_memory_rpc(s, q, sizeof(q), r, &len, budget_ms);
    if (status < 0)
        return status;
    uint64_t token = a20_work_u64(r + 40);
    if (!token || a20_work_u64(r + 32) != version) {
        a20_memory_close(s);
        return -A20_ERR_PROTOCOL;
    }
    *out = (a20_memory_ticket_t){
        .version = version, .token = token, .nonce = nonce, .page = page};
    a20_memcpy(out->owner, s->owner, 64);
    a20_memcpy(out->client, s->client, 64);
    return 0;
}
static inline a20_status_t
a20_memory_ticket_request(a20_memory_session_t *s, const a20_memory_ticket_t *t,
                          uint32_t op, uint8_t q[A20_MEMORY_HEADER]) {
    if (!s || !t || !t->version || !t->nonce || !t->token ||
        t->page >= s->config.pages || a20_memcmp(t->owner, s->owner, 64) ||
        a20_memcmp(t->client, s->client, 64))
        return -A20_ERR_EXPIRED;
    a20_memory_request(q, op, 1, s->owner, s->client);
    a20_work_put32(q + 16, t->page);
    a20_work_put64(q + 32, t->version);
    a20_work_put64(q + 40, t->token);
    a20_work_put64(q + 48, t->nonce);
    return 0;
}
static inline a20_status_t a20_memory_stage_page(a20_memory_session_t *s,
                                                 const a20_memory_ticket_t *t,
                                                 const void *buffer,
                                                 uint32_t budget_ms) {
    if (!buffer || !budget_ms || budget_ms > 30000)
        return -A20_ERR_INVALID_ARGUMENT;
    uint64_t start = a20_agent_clock_ms(0);
    uint8_t q[A20_MEMORY_MESSAGE], r[A20_MEMORY_MESSAGE];
    for (uint32_t offset = 0; offset < A20_MEMORY_PAGE;
         offset += A20_MEMORY_CHUNK) {
        a20_status_t status =
            a20_memory_ticket_request(s, t, A20_MEMORY_STAGE, q);
        if (status < 0)
            return status;
        a20_work_put32(q + 20, offset);
        a20_work_put32(q + 24, A20_MEMORY_CHUNK);
        a20_memcpy(q + A20_MEMORY_HEADER, (const uint8_t *)buffer + offset,
                   A20_MEMORY_CHUNK);
        uint32_t len, left = a20_memory_remaining(start, budget_ms);
        if (!left)
            return -A20_ERR_TIMED_OUT;
        status = a20_memory_rpc(s, q, sizeof(q), r, &len, left);
        if (status < 0)
            return status;
        if (a20_work_u64(r + 32) != t->version ||
            a20_work_u64(r + 40) != t->token) {
            a20_memory_close(s);
            return -A20_ERR_PROTOCOL;
        }
    }
    return 0;
}
static inline a20_status_t a20_memory_finish(a20_memory_session_t *s,
                                             const a20_memory_ticket_t *t,
                                             uint32_t op, uint64_t *new_version,
                                             uint32_t budget_ms) {
    if ((op != A20_MEMORY_COMMIT && op != A20_MEMORY_ABORT) ||
        (op == A20_MEMORY_COMMIT && !new_version))
        return -A20_ERR_INVALID_ARGUMENT;
    uint8_t q[A20_MEMORY_HEADER], r[A20_MEMORY_MESSAGE];
    a20_status_t status = a20_memory_ticket_request(s, t, op, q);
    if (status < 0)
        return status;
    uint32_t len;
    status = a20_memory_rpc(s, q, sizeof(q), r, &len, budget_ms);
    if (status < 0)
        return status;
    uint64_t expected = t->version + (op == A20_MEMORY_COMMIT);
    if (a20_work_u64(r + 32) != expected || a20_work_u64(r + 40) != t->token) {
        a20_memory_close(s);
        return -A20_ERR_PROTOCOL;
    }
    if (new_version)
        *new_version = expected;
    return 0;
}
#endif
