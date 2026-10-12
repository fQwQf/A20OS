#ifndef A20_CLUSTER_APPS_H
#define A20_CLUSTER_APPS_H
#include "runtime.h"
#include "execution.h"
#include "cluster_work.h"
/* One explicitly selected application owner. Fixed storage, no ISA-specific
 * launch policy, no transparent failover or tombstone eviction. */
#define A20_APP_SERVICE "instance.apps"
#define A20_APP_REQUEST 176u
#define A20_APP_REPLY 192u
#define A20_APP_HELLO 1u
#define A20_APP_START 2u
#define A20_APP_QUERY 3u
#define A20_APP_STOP 4u
#define A20_APP_OK 0u
#define A20_APP_INVALID 1u
#define A20_APP_STALE 2u
#define A20_APP_NOT_FOUND 3u
#define A20_APP_CONFLICT 4u
#define A20_APP_NO_SPACE 5u
#define A20_APP_NO_IMAGE 6u
#define A20_APP_IO 7u
#define A20_APP_BUSY 8u
#define A20_APP_RUNNING 1u
#define A20_APP_STOPPING 2u
#define A20_APP_EXITED 3u
#define A20_APP_FAILED 4u
#define A20_APP_LOST 5u
typedef struct a20_app_variant {
    uint8_t manifest[16];
    a20_execution_requirement_t requirement;
    uintptr_t image;
} a20_app_variant_t;
typedef struct a20_app_request {
    uint32_t op, reason;
    uint64_t cookie, nonce;
    a20_object_id_t controller, client;
    uint8_t manifest[16];
} a20_app_request_t;
typedef struct a20_app_result {
    uint32_t status, state, capacity, occupied, active;
    int32_t exit_code;
    a20_object_id_t controller, root;
    a20_execution_info_t execution;
} a20_app_result_t;
typedef struct a20_app_record {
    a20_object_id_t client, root;
    uint8_t manifest[16];
    uint64_t nonce;
    uint32_t state, stop_requested, has_object;
    int32_t exit_code;
    uintptr_t object;
} a20_app_record_t;
typedef struct a20_app_owner {
    a20_object_id_t controller;
    a20_execution_info_t execution;
    const a20_app_variant_t *variants;
    uint32_t variant_count, capacity;
    a20_app_record_t *records;
    void *ctx;
    /* start: <0 guarantees no application started; 0 returns a valid root;
     * >0 means started but identity observation failed. Preserve as LOST.
     * poll: 0 running, 1 exited, <0 unobservable (never infer death).
     * Successful start transfers one object reference to this owner. */
    int (*start)(void *, uintptr_t, uintptr_t *, a20_object_id_t *);
    int (*poll)(void *, uintptr_t, int32_t *);
    int (*stop)(void *, uintptr_t, uint32_t);
    void (*release)(void *, uintptr_t);
} a20_app_owner_t;
static inline int a20_app_zero(const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}
static inline int a20_app_request_decode(const uint8_t *p, uint32_t n,
                                         a20_app_request_t *out) {
    if (!p || !out || n != A20_APP_REQUEST || p[0] != 'C' || p[1] != 'A' ||
        p[2] != 1 || p[3] < 1 || p[3] > 4 || !a20_work_u64(p + 8) ||
        a20_work_u64(p + 24))
        return -1;
    a20_app_request_t q = {.op = p[3],
                           .reason = a20_work_u32(p + 4),
                           .cookie = a20_work_u64(p + 8),
                           .nonce = a20_work_u64(p + 16)};
    if (q.op == A20_APP_HELLO) {
        if (q.reason || q.nonce || !a20_app_zero(p + 32, 128))
            return -1;
        for (unsigned i = 0; i < 16; i++)
            q.manifest[i] = p[160 + i];
        if (!a20_app_zero(q.manifest, 16) && !a20_runtime_id_valid(q.manifest))
            return -1;
    } else {
        if (!q.nonce || a20_object_id_decode(p + 32, 64, &q.controller) ||
            a20_object_id_decode(p + 96, 64, &q.client))
            return -1;
        for (unsigned i = 0; i < 16; i++)
            q.manifest[i] = p[160 + i];
        if (!a20_runtime_id_valid(q.manifest) ||
            !a20_runtime_id_equal(q.client.instance_id,
                                  q.controller.instance_id) ||
            (q.op == A20_APP_QUERY && q.reason) ||
            (q.op == A20_APP_STOP && (!q.reason || q.reason > 63)))
            return -1;
    }
    *out = q;
    return 0;
}
static inline int a20_app_request_encode(uint8_t *p, uint32_t cap,
                                         const a20_app_request_t *q) {
    if (!p || cap < A20_APP_REQUEST || !q)
        return -1;
    for (unsigned i = 0; i < A20_APP_REQUEST; i++)
        p[i] = 0;
    p[0] = 'C';
    p[1] = 'A';
    p[2] = 1;
    p[3] = (uint8_t)q->op;
    a20_work_put32(p + 4, q->reason);
    a20_work_put64(p + 8, q->cookie);
    a20_work_put64(p + 16, q->nonce);
    if (q->op != A20_APP_HELLO) {
        if (a20_object_id_encode(p + 32, 64, &q->controller) ||
            a20_object_id_encode(p + 96, 64, &q->client))
            return -1;
    }
    for (unsigned i = 0; i < 16; i++)
        p[160 + i] = q->manifest[i];
    a20_app_request_t checked;
    return a20_app_request_decode(p, A20_APP_REQUEST, &checked);
}
static inline int a20_app_result_decode(const uint8_t *p, uint32_t n,
                                        const a20_app_request_t *q,
                                        a20_app_result_t *out) {
    if (!p || !q || !out || n != A20_APP_REPLY || p[0] != 'C' || p[1] != 'A' ||
        p[2] != 1 || p[3] != q->op || a20_work_u64(p + 8) != q->cookie ||
        a20_work_u64(p + 16) != q->nonce || a20_work_u32(p + 188))
        return -1;
    a20_app_result_t v = {.status = a20_work_u32(p + 4),
                          .state = a20_work_u32(p + 24),
                          .exit_code = (int32_t)a20_work_u32(p + 28)};
    v.capacity = a20_work_u32(p + 176);
    v.occupied = a20_work_u32(p + 180);
    v.active = a20_work_u32(p + 184);
    if (!v.capacity || v.occupied > v.capacity || v.active > v.occupied)
        return -1;
    if (v.status > A20_APP_BUSY ||
        a20_object_id_decode(p + 32, 64, &v.controller))
        return -1;
    for (unsigned i = 0; i < 16; i++)
        if (p[160 + i] != q->manifest[i])
            return -1;
    if (q->op == A20_APP_HELLO) {
        if (v.state || v.exit_code ||
            (v.status ? !a20_app_zero(p + 96, 64)
                      : a20_execution_decode(p + 96, 64, &v.execution)))
            return -1;
    } else if (v.status) {
        if (v.status != A20_APP_STALE &&
            !a20_object_id_equal(&v.controller, &q->controller))
            return -1;
        if (v.state || v.exit_code || !a20_app_zero(p + 96, 64))
            return -1;
    } else {
        if (!a20_object_id_equal(&v.controller, &q->controller) ||
            v.state < 1 || v.state > 5)
            return -1;
        int zero = a20_app_zero(p + 96, 64);
        if (v.state == A20_APP_FAILED) {
            if (!zero || v.exit_code >= 0)
                return -1;
        } else if (!zero) {
            if (a20_object_id_decode(p + 96, 64, &v.root) ||
                !a20_runtime_id_equal(v.root.instance_id,
                                      v.controller.instance_id) ||
                !a20_runtime_id_equal(v.root.domain_id,
                                      v.controller.domain_id) ||
                v.root.boot_epoch != v.controller.boot_epoch)
                return -1;
        } else if (v.state != A20_APP_LOST && v.state != A20_APP_STOPPING &&
                   v.state != A20_APP_EXITED)
            return -1;
        if ((v.state == A20_APP_RUNNING || v.state == A20_APP_STOPPING) &&
            v.exit_code)
            return -1;
    }
    *out = v;
    return 0;
}
static inline int a20_app_owner_init(
    a20_app_owner_t *o, const a20_object_id_t *controller,
    const a20_execution_info_t *execution, const a20_app_variant_t *variants,
    uint32_t count, a20_app_record_t *records, uint32_t capacity, void *ctx,
    int (*start)(void *, uintptr_t, uintptr_t *, a20_object_id_t *),
    int (*poll)(void *, uintptr_t, int32_t *),
    int (*stop)(void *, uintptr_t, uint32_t),
    void (*release)(void *, uintptr_t)) {
    if (!o || !a20_object_id_valid(controller) ||
        !a20_execution_valid(execution) || !variants || !count || !records ||
        !capacity || !start || !poll || !stop || !release)
        return -1;
    for (uint32_t i = 0; i < count; i++)
        if (!a20_runtime_id_valid(variants[i].manifest))
            return -1;
    *o = (a20_app_owner_t){.controller = *controller,
                           .execution = *execution,
                           .variants = variants,
                           .variant_count = count,
                           .records = records,
                           .capacity = capacity,
                           .ctx = ctx,
                           .start = start,
                           .poll = poll,
                           .stop = stop,
                           .release = release};
    for (uint32_t i = 0; i < capacity; i++)
        records[i] = (a20_app_record_t){0};
    return 0;
}
static inline void a20_app_owner_poll(a20_app_owner_t *o) {
    for (uint32_t i = 0; i < o->capacity; i++) {
        a20_app_record_t *r = &o->records[i];
        if (!r->nonce || !r->has_object)
            continue;
        int32_t code = 0;
        int state = o->poll(o->ctx, r->object, &code);
        if (state < 0)
            r->state = A20_APP_LOST;
        else if (state) {
            r->state = A20_APP_EXITED;
            r->exit_code = code;
            r->has_object = 0;
            o->release(o->ctx, r->object);
        } else if (a20_object_id_valid(&r->root))
            r->state = r->stop_requested ? A20_APP_STOPPING : A20_APP_RUNNING;
    }
}
static inline uint32_t a20_app_owner_variant(const a20_app_owner_t *o,
                                             const uint8_t manifest[16],
                                             const a20_app_variant_t **out) {
    const a20_app_variant_t *selected = 0;
    for (uint32_t i = 0; i < o->variant_count; i++) {
        const a20_app_variant_t *v = &o->variants[i];
        if (a20_runtime_id_equal(v->manifest, manifest) &&
            a20_execution_image_matches(&o->execution, &v->requirement)) {
            if (selected)
                return A20_APP_CONFLICT;
            selected = v;
        }
    }
    *out = selected;
    return selected ? A20_APP_OK : A20_APP_NO_IMAGE;
}
static inline void a20_app_owner_load(const a20_app_owner_t *o,
                                      uint32_t *occupied, uint32_t *active) {
    *occupied = *active = 0;
    for (uint32_t i = 0; i < o->capacity; i++) {
        if (o->records[i].nonce)
            (*occupied)++;
        if (o->records[i].has_object)
            (*active)++;
    }
}
/* Tombstones are retained until this controller incarnation ends. Full means
 * NO_SPACE, never silent eviction followed by duplicate side effects. */
static inline int a20_app_owner_dispatch(a20_app_owner_t *o,
                                         const uint8_t *input, uint32_t n,
                                         uint8_t *reply, uint32_t cap) {
    a20_app_request_t q;
    if (!o || !reply || cap < A20_APP_REPLY ||
        a20_app_request_decode(input, n, &q))
        return -1;
    a20_app_owner_poll(o);
    uint32_t status = 0;
    a20_app_record_t *found = 0;
    int free_index = -1;
    uint32_t occupied, active;
    a20_app_owner_load(o, &occupied, &active);
    if (q.op == A20_APP_HELLO && !a20_app_zero(q.manifest, 16)) {
        const a20_app_variant_t *selected = 0;
        status = a20_app_owner_variant(o, q.manifest, &selected);
    }
    if (q.op != A20_APP_HELLO) {
        if (!a20_object_id_equal(&q.controller, &o->controller))
            status = A20_APP_STALE;
        else {
            for (uint32_t i = 0; i < o->capacity; i++) {
                a20_app_record_t *r = &o->records[i];
                if (!r->nonce) {
                    if (free_index < 0)
                        free_index = (int)i;
                } else if (r->nonce == q.nonce &&
                           a20_object_id_equal(&r->client, &q.client))
                    found = r;
            }
            if (found && !a20_runtime_id_equal(found->manifest, q.manifest))
                status = A20_APP_CONFLICT;
            else if (q.op == A20_APP_START && !found) {
                const a20_app_variant_t *selected = 0;
                status = a20_app_owner_variant(o, q.manifest, &selected);
                if (!status && q.reason && active != q.reason - 1)
                    status = A20_APP_BUSY;
                if (!status && free_index < 0)
                    status = A20_APP_NO_SPACE;
                if (!status) {
                    found = &o->records[free_index];
                    *found = (a20_app_record_t){.client = q.client,
                                                .nonce = q.nonce};
                    for (unsigned i = 0; i < 16; i++)
                        found->manifest[i] = q.manifest[i];
                    int result = o->start(o->ctx, selected->image,
                                          &found->object, &found->root);
                    if (result < 0) {
                        found->state = A20_APP_FAILED;
                        found->exit_code = result;
                        found->root = (a20_object_id_t){0};
                    } else {
                        found->has_object = 1;
                        int valid =
                            a20_object_id_valid(&found->root) &&
                            a20_runtime_id_equal(found->root.instance_id,
                                                 o->controller.instance_id) &&
                            a20_runtime_id_equal(found->root.domain_id,
                                                 o->controller.domain_id) &&
                            found->root.boot_epoch == o->controller.boot_epoch;
                        found->state =
                            !result && valid ? A20_APP_RUNNING : A20_APP_LOST;
                        if (!valid)
                            found->root = (a20_object_id_t){0};
                    }
                }
            } else if (!found)
                status = A20_APP_NOT_FOUND;
            else if (q.op == A20_APP_STOP && found->has_object &&
                     !found->stop_requested) {
                if (o->stop(o->ctx, found->object, q.reason) < 0)
                    status = A20_APP_IO;
                else {
                    found->stop_requested = 1;
                    found->state = A20_APP_STOPPING;
                }
            }
        }
    }
    for (unsigned i = 0; i < A20_APP_REPLY; i++)
        reply[i] = 0;
    reply[0] = 'C';
    reply[1] = 'A';
    reply[2] = 1;
    reply[3] = (uint8_t)q.op;
    a20_work_put32(reply + 4, status);
    a20_work_put64(reply + 8, q.cookie);
    a20_work_put64(reply + 16, q.nonce);
    a20_object_id_encode(reply + 32, 64, &o->controller);
    for (unsigned i = 0; i < 16; i++)
        reply[160 + i] = q.manifest[i];
    if (!status) {
        if (q.op == A20_APP_HELLO)
            a20_execution_encode(reply + 96, 64, &o->execution);
        else {
            a20_work_put32(reply + 24, found->state);
            a20_work_put32(reply + 28, (uint32_t)found->exit_code);
            if (a20_object_id_valid(&found->root))
                a20_object_id_encode(reply + 96, 64, &found->root);
        }
    }
    a20_app_owner_load(o, &occupied, &active);
    a20_work_put32(reply + 176, o->capacity);
    a20_work_put32(reply + 180, occupied);
    a20_work_put32(reply + 184, active);
    return A20_APP_REPLY;
}
#endif
