#ifndef _CLUSTER_FRAGMENT_H
#define _CLUSTER_FRAGMENT_H

#include "cluster/frame.h"
#include "cluster/transaction.h"

/* v1 only: session pair, total logical payload bytes, body offset, chunk.
 * The body excludes the repeated session pair. Physical MTU fixes chunk size.
 * Peers must negotiate FRAGMENT before emitting this format. */
#define A20_CLX_FRAGMENT_PREFIX 24u
#define A20_CLX_FRAGMENT_COUNT 64u
#define A20_CLX_FRAGMENT_TTL_MS 30000u

typedef struct a20_clx_fragment_part {
    const a20_frame_hdr_t *header;
    const uint8_t *payload;
    uint32_t total, offset, chunk, length, count, index;
} a20_clx_fragment_part_t;

typedef struct a20_clx_assembly {
    a20_frame_hdr_t header;
    uint64_t source_epoch, destination_epoch, received;
    uint32_t total, chunk;
} a20_clx_assembly_t;

/* Validate all arithmetic before the caller reserves memory. */
static inline int a20_clx_fragment_parse(const a20_frame_hdr_t *h,
                                         const uint8_t *p, uint32_t mtu,
                                         uint32_t logical_mtu,
                                         a20_clx_fragment_part_t *out) {
    if (!h || !p || !out || h->ver != A20_CLX_SESSION_VER ||
        h->flags != (A20_CLX_FLAG_RELIABLE | A20_CLX_FLAG_FRAGMENTED) ||
        (h->type != A20_CLX_TYPE_CALL && h->type != A20_CLX_TYPE_CALL_REPLY &&
         h->type != A20_CLX_TYPE_SEND) ||
        !h->seq || h->seq == A20_CLX_CLOSE_CALLER ||
        (h->type == A20_CLX_TYPE_SEND ? h->txid || !h->dst_slot
                                      : h->seq != h->txid) ||
        (h->type == A20_CLX_TYPE_CALL && !h->dst_slot) ||
        h->csum_kind != A20_CLX_CSUM_CCITT || h->frag & A20_CLX_FRAG_RESERVED ||
        h->payload_len <= A20_CLX_FRAGMENT_PREFIX ||
        a20_frame_max_payload(mtu) <= A20_CLX_FRAGMENT_PREFIX ||
        h->payload_len > a20_frame_max_payload(mtu))
        return -1;
    uint32_t total = a20_clx_get_le32(p + 16);
    uint32_t offset = a20_clx_get_le32(p + 20);
    uint32_t chunk = a20_frame_max_payload(mtu) - A20_CLX_FRAGMENT_PREFIX;
    uint32_t index = h->frag & A20_CLX_FRAG_SEQ_MASK;
    if (total <= a20_frame_max_payload(mtu) ||
        total > a20_frame_max_payload(logical_mtu) ||
        total <= A20_CLX_SESSION_BYTES || index >= A20_CLX_FRAGMENT_COUNT ||
        offset != index * chunk || offset >= total - A20_CLX_SESSION_BYTES)
        return -1;
    uint32_t remaining = total - A20_CLX_SESSION_BYTES - offset;
    uint32_t length = remaining < chunk ? remaining : chunk;
    uint32_t count = (total - A20_CLX_SESSION_BYTES + chunk - 1) / chunk;
    if (count > A20_CLX_FRAGMENT_COUNT ||
        h->payload_len != A20_CLX_FRAGMENT_PREFIX + length ||
        !!(h->frag & A20_CLX_FRAG_LAST) != (index + 1 == count))
        return -1;
    *out = (a20_clx_fragment_part_t){h,     p,      total, offset,
                                     chunk, length, count, index};
    return 0;
}

/* Caller serializes one assembly and provides exactly bounded storage.
 * Return 1 complete, 0 incomplete/identical duplicate, -1 conflicting data.
 * received==0 denotes a new assembly. Never extend its external deadline. */
static inline int a20_clx_fragment_feed(a20_clx_assembly_t *a,
                                        const a20_clx_fragment_part_t *part,
                                        uint8_t *storage, uint32_t capacity) {
    if (!a || !part || !storage || capacity < part->total)
        return -1;
    const a20_frame_hdr_t *h = part->header;
    uint64_t source = a20_clx_get_le64(part->payload);
    uint64_t destination = a20_clx_get_le64(part->payload + 8);
    if (!a->received) {
        a->header = *h;
        a->header.flags &= ~A20_CLX_FLAG_FRAGMENTED;
        a->header.frag = 0;
        a->header.payload_len = part->total;
        a->source_epoch = source;
        a->destination_epoch = destination;
        a->total = part->total;
        a->chunk = part->chunk;
        a20_clx_session_encode(storage, source, destination);
    } else if (a->total != part->total || a->chunk != part->chunk ||
               a->source_epoch != source ||
               a->destination_epoch != destination ||
               a->header.type != h->type || a->header.txid != h->txid ||
               a->header.src_hash != h->src_hash ||
               a->header.dst_hash != h->dst_hash ||
               a->header.dst_slot != h->dst_slot || a->header.seq != h->seq) {
        return -1;
    }
    uint64_t bit = 1ULL << part->index;
    uint8_t *target = storage + A20_CLX_SESSION_BYTES + part->offset;
    const uint8_t *data = part->payload + A20_CLX_FRAGMENT_PREFIX;
    if (a->received & bit) {
        for (uint32_t i = 0; i < part->length; i++)
            if (target[i] != data[i])
                return -1;
    } else {
        for (uint32_t i = 0; i < part->length; i++)
            target[i] = data[i];
        a->received |= bit;
    }
    uint64_t expected =
        part->count == 64 ? ~(uint64_t)0 : (1ULL << part->count) - 1;
    return a->received == expected;
}

#endif
