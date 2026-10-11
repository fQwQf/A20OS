#ifndef A20_CLUSTER_DIRECTORY_H
#define A20_CLUSTER_DIRECTORY_H
#include "cluster_work.h"

/* Stable read-only discovery endpoint, independent of user-daemon lifetime.
 * Export slots below it are never reused during a boot. */
#define A20_DIRECTORY_WORK (1u << 2) /* Same bit as Native A20_EXPORT_WORK. */
#define A20_DIRECTORY_SLOT 0xfffffffeu
#define A20_DIRECTORY_REQUEST 16u
#define A20_DIRECTORY_REPLY 128u
#define A20_DIRECTORY_FOUND 0u
#define A20_DIRECTORY_END 1u
#define A20_DIRECTORY_CHANGED 2u
#define A20_DIRECTORY_INVALID 3u

static inline void a20_directory_request(uint8_t out[16], uint32_t index,
                                         uint64_t generation) {
    out[0] = 'C';
    out[1] = 'D';
    out[2] = 1;
    out[3] = 0;
    a20_work_put32(out + 4, index);
    a20_work_put64(out + 8, generation);
}
static inline int a20_directory_decode_request(const uint8_t *p, uint32_t len,
                                               uint32_t *index,
                                               uint64_t *generation) {
    if (!p || len != 16 || p[0] != 'C' || p[1] != 'D' || p[2] != 1 || p[3])
        return -1;
    *index = a20_work_u32(p + 4);
    *generation = a20_work_u64(p + 8);
    return 0;
}

typedef struct a20_directory_record {
    uint8_t status, profile;
    uint8_t node_id[16];
    uint64_t generation, boot_epoch;
    uint32_t next_index, caps, slot, flags, name_len;
    char name[64];
} a20_directory_record_t;

static inline void a20_directory_encode(uint8_t p[128],
                                        const a20_directory_record_t *r) {
    for (unsigned i = 0; i < 128; i++)
        p[i] = 0;
    p[0] = 'C';
    p[1] = 'D';
    p[2] = 1;
    p[3] = r->status;
    a20_work_put64(p + 4, r->generation);
    a20_work_put32(p + 12, r->next_index);
    for (unsigned i = 0; i < 16; i++)
        p[16 + i] = r->node_id[i];
    a20_work_put64(p + 32, r->boot_epoch);
    a20_work_put32(p + 40, r->caps);
    a20_work_put32(p + 44, r->profile);
    a20_work_put32(p + 48, r->slot);
    a20_work_put32(p + 52, r->flags);
    uint32_t n = r->name_len <= 64 ? r->name_len : 0;
    a20_work_put32(p + 56, n);
    for (unsigned i = 0; i < n; i++)
        p[60 + i] = (uint8_t)r->name[i];
}
static inline int a20_directory_decode(const uint8_t *p, uint32_t len,
                                       const uint8_t expected_node[16],
                                       a20_directory_record_t *r) {
    if (!p || !r || len != 128 || p[0] != 'C' || p[1] != 'D' || p[2] != 1 ||
        p[3] > A20_DIRECTORY_INVALID || a20_work_u32(p + 56) > 64 ||
        a20_work_u32(p + 124) || !a20_work_u64(p + 32) ||
        a20_work_u32(p + 44) < 1 || a20_work_u32(p + 44) > 3)
        return -1;
    for (unsigned i = 0; i < 16; i++) {
        if (expected_node && p[16 + i] != expected_node[i])
            return -1;
        r->node_id[i] = p[16 + i];
    }
    r->status = p[3];
    r->generation = a20_work_u64(p + 4);
    r->next_index = a20_work_u32(p + 12);
    r->boot_epoch = a20_work_u64(p + 32);
    r->caps = a20_work_u32(p + 40);
    r->profile = (uint8_t)a20_work_u32(p + 44);
    r->slot = a20_work_u32(p + 48);
    r->flags = a20_work_u32(p + 52);
    r->name_len = a20_work_u32(p + 56);
    for (unsigned i = 0; i < r->name_len; i++)
        r->name[i] = (char)p[60 + i];
    if (r->status == A20_DIRECTORY_FOUND &&
        (!r->slot || r->slot >= A20_DIRECTORY_SLOT || !r->name_len ||
         !r->next_index))
        return -1;
    return 0;
}
#endif
