#ifndef A20_CLUSTER_OBJECTS_H
#define A20_CLUSTER_OBJECTS_H
#include "runtime.h"
#include "cluster_work.h"
/* Allocation-free owner shard. IDs locate objects; only a local transferred
 * capability can publish one. No process-local handle/PID crosses the wire. */
#define A20_OBJECT_SERVICE "instance.objects"
#define A20_OBJECT_REQUEST 96u
#define A20_OBJECT_REPLY 96u
#define A20_OBJECT_QUERY 1u
#define A20_OBJECT_PUBLISH 2u
#define A20_OBJECT_RENEW 3u
#define A20_OBJECT_REVOKE 4u
#define A20_OBJECT_OK 0u
#define A20_OBJECT_INVALID 1u
#define A20_OBJECT_NOT_FOUND 2u
#define A20_OBJECT_EXPIRED 3u
#define A20_OBJECT_DENIED 4u
#define A20_OBJECT_NO_SPACE 5u
#define A20_OBJECT_CONFLICT 6u
#define A20_OBJECT_STALE 7u
#define A20_OBJECT_FAILED 8u
#define A20_OBJECT_MAX_LEASE 30000u
typedef struct a20_object_message {
    uint32_t op, value, state;
    uint64_t cookie, publication;
    a20_object_id_t id;
} a20_object_message_t;
static inline int a20_object_request_encode(uint8_t *p, uint32_t cap,
                                            const a20_object_message_t *q) {
    if (!p || cap < 96 || !q || q->op < 1 || q->op > 4 || !q->cookie ||
        !a20_object_id_valid(&q->id))
        return -1;
    for (unsigned i = 0; i < 96; i++)
        p[i] = 0;
    p[0] = 'C';
    p[1] = 'O';
    p[2] = 1;
    p[3] = (uint8_t)q->op;
    a20_work_put32(p + 4, q->value);
    a20_work_put64(p + 8, q->cookie);
    a20_work_put64(p + 16, q->publication);
    return a20_object_id_encode(p + 32, 64, &q->id);
}
static inline int a20_object_request_decode(const uint8_t *p, uint32_t n,
                                            a20_object_message_t *out) {
    if (!p || !out || n != 96 || p[0] != 'C' || p[1] != 'O' || p[2] != 1 ||
        p[3] < 1 || p[3] > 4 || !a20_work_u64(p + 8) || a20_work_u64(p + 24))
        return -1;
    a20_object_message_t q = {.op = p[3],
                              .value = a20_work_u32(p + 4),
                              .cookie = a20_work_u64(p + 8),
                              .publication = a20_work_u64(p + 16)};
    if (a20_object_id_decode(p + 32, 64, &q.id))
        return -1;
    if ((q.op == A20_OBJECT_QUERY || q.op == A20_OBJECT_REVOKE) && q.value)
        return -1;
    if ((q.op == A20_OBJECT_PUBLISH || q.op == A20_OBJECT_RENEW) &&
        (!q.value || q.value > A20_OBJECT_MAX_LEASE))
        return -1;
    if (q.op == A20_OBJECT_PUBLISH && q.publication)
        return -1;
    if ((q.op == A20_OBJECT_RENEW || q.op == A20_OBJECT_REVOKE) &&
        !q.publication)
        return -1;
    *out = q;
    return 0;
}
static inline int a20_object_reply_decode(const uint8_t *p, uint32_t n,
                                          const a20_object_message_t *q,
                                          a20_object_message_t *out) {
    if (!p || !q || !out || n != 96 || p[0] != 'C' || p[1] != 'O' ||
        p[2] != 1 || p[3] != q->op || a20_work_u64(p + 8) != q->cookie ||
        a20_work_u32(p + 4) > A20_OBJECT_FAILED)
        return -1;
    a20_object_message_t v = {.op = p[3],
                              .value = a20_work_u32(p + 4),
                              .cookie = a20_work_u64(p + 8),
                              .publication = a20_work_u64(p + 16),
                              .state = a20_work_u32(p + 28)};
    if (a20_object_id_decode(p + 32, 64, &v.id) ||
        !a20_object_id_equal(&v.id, &q->id))
        return -1;
    uint32_t remaining = a20_work_u32(p + 24);
    if (v.value) {
        if (v.publication || v.state || remaining)
            return -1;
    } else if (q->op == A20_OBJECT_REVOKE) {
        if (v.publication || v.state || remaining)
            return -1;
    } else if (!v.publication || !remaining ||
               remaining > A20_OBJECT_MAX_LEASE ||
               (v.state != A20_OBJECT_LIVE && v.state != A20_OBJECT_RETIRED) ||
               (q->publication && v.publication != q->publication))
        return -1;
    *out = v;
    return 0;
}
typedef struct a20_object_record {
    a20_object_id_t id;
    uint64_t publication, owner, deadline;
    uintptr_t object;
} a20_object_record_t;
typedef struct a20_object_directory {
    a20_runtime_info_t runtime;
    a20_object_record_t *records;
    uint32_t capacity;
    uint64_t serial;
    void *ctx;
    int (*identify)(void *, uintptr_t, a20_handle_identity_t *);
    void (*release)(void *, uintptr_t);
} a20_object_directory_t;
static inline int a20_object_directory_init(
    a20_object_directory_t *d, const a20_runtime_info_t *runtime,
    a20_object_record_t *records, uint32_t count,
    int (*identify)(void *, uintptr_t, a20_handle_identity_t *),
    void (*release)(void *, uintptr_t), void *ctx) {
    if (!d || !a20_runtime_info_valid(runtime) ||
        runtime->state != A20_RUNTIME_BOUND || !records || !count ||
        !identify || !release)
        return -1;
    *d = (a20_object_directory_t){.runtime = *runtime,
                                  .records = records,
                                  .capacity = count,
                                  .identify = identify,
                                  .release = release,
                                  .ctx = ctx};
    for (uint32_t i = 0; i < count; i++)
        records[i] = (a20_object_record_t){0};
    return 0;
}
static inline void a20_object_directory_remove(a20_object_directory_t *d,
                                               uint32_t i) {
    a20_object_record_t *s = &d->records[i];
    if (s->publication) {
        d->release(d->ctx, s->object);
        *s = (a20_object_record_t){0};
    }
}
static inline void a20_object_directory_expire(a20_object_directory_t *d,
                                               uint64_t now) {
    for (uint32_t i = 0; i < d->capacity; i++)
        if (d->records[i].publication && now >= d->records[i].deadline)
            a20_object_directory_remove(d, i);
}
static inline void a20_object_directory_disconnect(a20_object_directory_t *d,
                                                   uint64_t owner) {
    if (!owner)
        return;
    for (uint32_t i = 0; i < d->capacity; i++)
        if (d->records[i].publication && d->records[i].owner == owner)
            a20_object_directory_remove(d, i);
}
/* Always consumes an attached object, including malformed/denied requests.
 * owner=0 is a remote/read-only connection; local adapters supply a stable
 * connection incarnation and call disconnect before that number is reused. */
static inline int a20_object_directory_dispatch(a20_object_directory_t *d,
                                                uint64_t owner,
                                                uintptr_t object, int attached,
                                                const uint8_t *input,
                                                uint32_t n, uint8_t *reply,
                                                uint32_t cap, uint64_t now) {
    a20_object_message_t q;
    int valid = cap >= 96 && reply && !a20_object_request_decode(input, n, &q);
    if (!valid) {
        if (attached)
            d->release(d->ctx, object);
        return -1;
    }
    a20_object_directory_expire(d, now);
    uint32_t status = A20_OBJECT_OK, state = 0, remaining = 0;
    uint64_t publication = 0;
    a20_object_record_t *found = 0;
    int free_index = -1;
    for (uint32_t i = 0; i < d->capacity; i++) {
        if (!d->records[i].publication) {
            if (free_index < 0)
                free_index = (int)i;
        } else if (a20_object_id_equal(&q.id, &d->records[i].id))
            found = &d->records[i];
    }
    if (!a20_runtime_id_equal(q.id.instance_id, d->runtime.instance_id) ||
        !a20_runtime_id_equal(q.id.domain_id, d->runtime.domain_id) ||
        q.id.boot_epoch != d->runtime.boot_epoch)
        status = A20_OBJECT_STALE;
    else if ((q.op == A20_OBJECT_PUBLISH || q.op == A20_OBJECT_RENEW) &&
             now > UINT64_MAX - q.value)
        status = A20_OBJECT_INVALID;
    else if (q.op == A20_OBJECT_PUBLISH) {
        a20_handle_identity_t identity = {0};
        if (!owner || !attached)
            status = A20_OBJECT_DENIED;
        else if (d->identify(d->ctx, object, &identity) ||
                 !a20_object_id_equal(&q.id, &identity.id) ||
                 (identity.state != A20_OBJECT_LIVE &&
                  identity.state != A20_OBJECT_RETIRED))
            status = A20_OBJECT_DENIED;
        else if (found && found->owner != owner)
            status = A20_OBJECT_CONFLICT;
        else if (!found && (free_index < 0 || d->serial == UINT64_MAX))
            status = A20_OBJECT_NO_SPACE;
        else {
            if (!found) {
                found = &d->records[free_index];
                *found = (a20_object_record_t){.id = q.id,
                                               .publication = ++d->serial,
                                               .owner = owner,
                                               .object = object};
                attached = 0;
            }
            found->deadline = now + q.value;
            state = identity.state;
        }
    } else if (attached)
        status = A20_OBJECT_INVALID;
    else if (!found)
        status = A20_OBJECT_NOT_FOUND;
    else if (q.publication && q.publication != found->publication)
        status = A20_OBJECT_EXPIRED;
    else if (q.op != A20_OBJECT_QUERY && (!owner || found->owner != owner))
        status = A20_OBJECT_DENIED;
    else if (q.op == A20_OBJECT_REVOKE) {
        uint32_t index = (uint32_t)(found - d->records);
        a20_object_directory_remove(d, index);
        found = 0;
    } else {
        a20_handle_identity_t identity = {0};
        if (d->identify(d->ctx, found->object, &identity) ||
            !a20_object_id_equal(&q.id, &identity.id) ||
            (identity.state != A20_OBJECT_LIVE &&
             identity.state != A20_OBJECT_RETIRED))
            status = A20_OBJECT_FAILED;
        else {
            state = identity.state;
            if (q.op == A20_OBJECT_RENEW)
                found->deadline = now + q.value;
        }
    }
    if (attached)
        d->release(d->ctx, object);
    if (!status && found) {
        publication = found->publication;
        remaining = (uint32_t)(found->deadline - now);
    }
    for (unsigned i = 0; i < 96; i++)
        reply[i] = 0;
    reply[0] = 'C';
    reply[1] = 'O';
    reply[2] = 1;
    reply[3] = (uint8_t)q.op;
    a20_work_put32(reply + 4, status);
    a20_work_put64(reply + 8, q.cookie);
    if (!status) {
        a20_work_put64(reply + 16, publication);
        a20_work_put32(reply + 24, remaining);
        a20_work_put32(reply + 28, state);
    }
    a20_object_id_encode(reply + 32, 64, &q.id);
    return 96;
}
#endif
