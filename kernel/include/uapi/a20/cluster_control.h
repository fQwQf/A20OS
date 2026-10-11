#ifndef A20_CLUSTER_CONTROL_H
#define A20_CLUSTER_CONTROL_H
#include "cluster_membership.h"
#define A20_CONTROL_REQUEST 96u
#define A20_CONTROL_REPLY 80u
#define A20_CONTROL_RESOLVE 1u
#define A20_CONTROL_MEMBER 2u
#define A20_CONTROL_INDIRECT 3u
/* CM/1 requests: type at 3, work kind/member index at 4, operator at 8,
 * input bytes at 12, name bytes at 16, reserved at 20, cookie at 24,
 * name/target id at 32. Replies: status at 4, node at 8, epoch at 24,
 * slot/state/caps/profile at 32/36/40/44, input/output/op-flags/inflight
 * at 48/52/56/60, cookie/incarnation at 64/72. */
static inline void a20_control_request(uint8_t out[96], uint8_t type,
                                       uint32_t kind, uint32_t op,
                                       uint32_t input_bytes, const char *name,
                                       uint32_t name_len, uint64_t cookie) {
    for (unsigned i = 0; i < 96; i++)
        out[i] = 0;
    out[0] = 'C';
    out[1] = 'M';
    out[2] = 1;
    out[3] = type;
    a20_work_put32(out + 4, kind);
    a20_work_put32(out + 8, op);
    a20_work_put32(out + 12, input_bytes);
    a20_work_put32(out + 16, name_len);
    a20_work_put64(out + 24, cookie);
    if (name && name_len <= 64)
        for (unsigned i = 0; i < name_len; i++)
            out[32 + i] = (uint8_t)name[i];
}
static inline int a20_control_valid(const uint8_t *p, uint32_t len) {
    return p && len == 96 && p[0] == 'C' && p[1] == 'M' && p[2] == 1 &&
           p[3] >= 1 && p[3] <= 3 && !a20_work_u32(p + 20) &&
           a20_work_u32(p + 16) <= 64;
}
#endif
