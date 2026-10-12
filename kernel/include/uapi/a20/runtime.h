#ifndef A20_RUNTIME_H
#define A20_RUNTIME_H
#include <stdint.h>
/* Explicitly provisioned namespace, immutable for this native kernel boot.
 * This is identity/configuration state, not a cluster health or JVM readiness
 * assertion. No kernel pointer/PID is a transferable object identity. */
#define A20_RUNTIME_VERSION 1u
#define A20_RUNTIME_BYTES 64u
#define A20_RUNTIME_UNBOUND 0u
#define A20_RUNTIME_BOUND 1u
#define A20_OBJECT_ID_BYTES 64u
#define A20_HANDLE_IDENTITY_BYTES 80u
#define A20_OBJECT_KIND_TASK 1u
#define A20_OBJECT_LIVE 1u
#define A20_OBJECT_RETIRED 2u

typedef struct a20_runtime_binding {
    uint32_t size, version, flags, reserved;
    uint8_t instance_id[16], domain_id[16];
    uint64_t boot_epoch, reserved64;
} a20_runtime_binding_t;
typedef struct a20_runtime_info {
    uint32_t size, version, state, flags;
    uint8_t instance_id[16], domain_id[16];
    uint64_t boot_epoch, reserved64;
} a20_runtime_info_t;
typedef struct a20_object_id {
    uint8_t instance_id[16], domain_id[16];
    uint64_t boot_epoch, serial;
    uint32_t kind, reserved[3];
} a20_object_id_t;
typedef struct a20_handle_identity {
    uint32_t size, version, state, reserved;
    a20_object_id_t id;
} a20_handle_identity_t;

static inline int a20_runtime_id_valid(const uint8_t id[16]) {
    unsigned any = 0, all = 255;
    if (!id)
        return 0;
    for (unsigned i = 0; i < 16; ++i) {
        any |= id[i];
        all &= id[i];
    }
    return any && all != 255;
}
static inline int a20_runtime_id_equal(const uint8_t a[16],
                                       const uint8_t b[16]) {
    for (unsigned i = 0; i < 16; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static inline int a20_runtime_binding_valid(const a20_runtime_binding_t *b) {
    return b && b->size == A20_RUNTIME_BYTES &&
           b->version == A20_RUNTIME_VERSION && !b->flags && !b->reserved &&
           !b->reserved64 && b->boot_epoch &&
           a20_runtime_id_valid(b->instance_id) &&
           a20_runtime_id_valid(b->domain_id);
}
static inline int a20_runtime_info_valid(const a20_runtime_info_t *i) {
    if (!i || i->size != A20_RUNTIME_BYTES ||
        i->version != A20_RUNTIME_VERSION || i->flags || i->reserved64 ||
        i->state > A20_RUNTIME_BOUND)
        return 0;
    if (i->state == A20_RUNTIME_BOUND)
        return i->boot_epoch && a20_runtime_id_valid(i->instance_id) &&
               a20_runtime_id_valid(i->domain_id);
    if (i->boot_epoch)
        return 0;
    for (unsigned k = 0; k < 16; ++k)
        if (i->instance_id[k] || i->domain_id[k])
            return 0;
    return 1;
}
static inline int a20_object_id_valid(const a20_object_id_t *id) {
    return id && a20_runtime_id_valid(id->instance_id) &&
           a20_runtime_id_valid(id->domain_id) && id->boot_epoch &&
           id->serial && id->kind == A20_OBJECT_KIND_TASK && !id->reserved[0] &&
           !id->reserved[1] && !id->reserved[2];
}
static inline int a20_object_id_equal(const a20_object_id_t *a,
                                      const a20_object_id_t *b) {
    return a20_object_id_valid(a) && a20_object_id_valid(b) &&
           a20_runtime_id_equal(a->instance_id, b->instance_id) &&
           a20_runtime_id_equal(a->domain_id, b->domain_id) &&
           a->boot_epoch == b->boot_epoch && a->serial == b->serial &&
           a->kind == b->kind;
}
/* Explicit LE codecs. The ID is a name, never authority to open a capability. */
static inline void a20_runtime_put64(uint8_t *p, uint64_t n) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(n >> (i * 8));
}
static inline uint64_t a20_runtime_get64(const uint8_t *p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i)
        n |= (uint64_t)p[i] << (i * 8);
    return n;
}
static inline int a20_object_id_encode(uint8_t *p, uint32_t cap,
                                       const a20_object_id_t *id) {
    if (!p || cap < A20_OBJECT_ID_BYTES || !a20_object_id_valid(id))
        return -1;
    for (unsigned i = 0; i < 16; ++i) {
        p[i] = id->instance_id[i];
        p[i + 16] = id->domain_id[i];
    }
    a20_runtime_put64(p + 32, id->boot_epoch);
    a20_runtime_put64(p + 40, id->serial);
    for (unsigned i = 48; i < 64; ++i)
        p[i] = 0;
    p[48] = A20_OBJECT_KIND_TASK;
    return 0;
}
static inline int a20_object_id_decode(const uint8_t *p, uint32_t len,
                                       a20_object_id_t *out) {
    if (!p || !out || len != A20_OBJECT_ID_BYTES ||
        p[48] != A20_OBJECT_KIND_TASK)
        return -1;
    for (unsigned i = 49; i < 64; ++i)
        if (p[i])
            return -1;
    a20_object_id_t id = {.boot_epoch = a20_runtime_get64(p + 32),
                          .serial = a20_runtime_get64(p + 40),
                          .kind = p[48]};
    for (unsigned i = 0; i < 16; ++i) {
        id.instance_id[i] = p[i];
        id.domain_id[i] = p[i + 16];
    }
    if (!a20_object_id_valid(&id))
        return -1;
    *out = id;
    return 0;
}
#endif
