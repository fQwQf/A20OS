/* Allocation-free end-to-end session control, shared by hosted and MCU peers.
 * Link HELLO authenticates adjacency only. CS challenges bind a routed identity
 * to its boot epoch and probe the smallest bidirectional physical MTU.
 * This is replay resistance on a trusted cluster, not cryptographic identity. */
#ifndef _CLUSTER_SESSION_CONTROL_H
#define _CLUSTER_SESSION_CONTROL_H
#include "cluster/frame.h"
#include "cluster/transaction.h"

#define A20_CS_SLOT 0xfffffffdu
#define A20_CS_BYTES 72u
#define A20_CS_PROBE 1u
#define A20_CS_REPLY 2u
#define A20_CS_RETRY_MS 250u
#define A20_CS_REFRESH_MS 5000u
#define A20_CS_LEASE_MS 10000u
#define A20_CS_RETRIES 8u

typedef struct a20_cs_record {
    uint32_t kind, mtu, caps, profile;
    uint64_t cookie, epoch, target_epoch;
    uint8_t source[16], target[16];
} a20_cs_record_t;

typedef struct a20_cs_session {
    uint64_t epoch, cookie, sent_ms, confirmed_ms;
    uint32_t caps, profile, mtu, retries;
    int confirmed;
} a20_cs_session_t;

static inline void a20_cs_encode(uint8_t p[A20_CS_BYTES],
                                 const a20_cs_record_t *r) {
    p[0] = 'C';
    p[1] = 'S';
    p[2] = 1;
    p[3] = r->kind;
    a20_clx_put_le32(p + 4, r->mtu);
    a20_clx_put_le64(p + 8, r->cookie);
    for (uint32_t i = 0; i < 16; i++) {
        p[16 + i] = r->source[i];
        p[48 + i] = r->target[i];
    }
    a20_clx_put_le64(p + 32, r->epoch);
    a20_clx_put_le32(p + 40, r->caps);
    a20_clx_put_le32(p + 44, r->profile);
    a20_clx_put_le64(p + 64, r->target_epoch);
}

static inline int a20_cs_decode(const a20_frame_hdr_t *h, const uint8_t *p,
                                a20_cs_record_t *r) {
    if (!h || !p || !r || h->ver != 1 || h->type != A20_CLX_TYPE_SEND ||
        h->dst_slot != A20_CS_SLOT || h->payload_len != A20_CS_BYTES ||
        h->flags || h->frag || h->txid || h->seq || !h->ttl ||
        h->csum_kind != A20_CLX_CSUM_CCITT || p[0] != 'C' || p[1] != 'S' ||
        p[2] != 1 || (p[3] != A20_CS_PROBE && p[3] != A20_CS_REPLY))
        return -1;
    r->kind = p[3];
    r->mtu = a20_clx_get_le32(p + 4);
    r->cookie = a20_clx_get_le64(p + 8);
    r->epoch = a20_clx_get_le64(p + 32);
    r->caps = a20_clx_get_le32(p + 40);
    r->profile = a20_clx_get_le32(p + 44);
    r->target_epoch = a20_clx_get_le64(p + 64);
    int source_zero = 1, source_ff = 1, target_zero = 1, target_ff = 1;
    for (uint32_t i = 0; i < 16; i++) {
        r->source[i] = p[16 + i];
        r->target[i] = p[48 + i];
        source_zero &= r->source[i] == 0;
        source_ff &= r->source[i] == 255;
        target_zero &= r->target[i] == 0;
        target_ff &= r->target[i] == 255;
    }
    if (source_zero || source_ff || target_zero || target_ff || !r->cookie ||
        !r->epoch ||
        r->mtu < A20_CLX_HDR_LEN + A20_CLX_CRC_LEN + A20_CS_BYTES ||
        r->mtu > 65536 || r->profile < 1 || r->profile > 3 ||
        !(r->caps & A20_CLX_CAP_RELIABLE) || r->caps & ~15u ||
        ((r->caps & A20_CLX_CAP_FRAGMENT) && r->profile == 1) ||
        (r->kind == A20_CS_PROBE && r->target_epoch) ||
        (r->kind == A20_CS_REPLY && !r->target_epoch) ||
        a20_clx_fnv1a32(r->source) != h->src_hash ||
        a20_clx_fnv1a32(r->target) != h->dst_hash || h->src_hash == h->dst_hash)
        return -1;
    return 0;
}

/* A fresh cookie is never reused within one boot. Replies also bind the
 * requester's epoch, so a previous boot's reply cannot satisfy this boot. */
static inline int a20_cs_begin(a20_cs_session_t *s, uint64_t *counter,
                               uint64_t now) {
    if (s->cookie || *counter == UINT64_MAX)
        return -1;
    s->cookie = ++*counter;
    s->sent_ms = now;
    s->retries = 0;
    return 0;
}
static inline int a20_cs_accept(a20_cs_session_t *s, const a20_cs_record_t *r,
                                uint64_t local, uint64_t now) {
    if (r->kind != A20_CS_REPLY || !s->cookie || r->cookie != s->cookie ||
        r->target_epoch != local)
        return -1;
    int changed = s->epoch && s->epoch != r->epoch;
    s->cookie = 0;
    s->epoch = r->epoch;
    s->caps = r->caps;
    s->profile = r->profile;
    s->mtu = r->mtu;
    s->confirmed_ms = now;
    s->confirmed = 1;
    return changed;
}
static inline int a20_cs_live(const a20_cs_session_t *s, uint64_t now) {
    return s->confirmed && now - s->confirmed_ms < A20_CS_LEASE_MS;
}
#endif
