/*
 * Cluster wire-frame codec (docs/cluster/02-wire-protocol.md §1-§2).
 *
 * Implementation contract (docs/cluster/03-kernel-impl.md §2 frame.c row):
 * pure functions, no locks, no allocation, the caller owns every buffer.
 * Every multi-byte field goes through the explicit little-endian accessors
 * of kernel/cluster/uart.h; the 32-byte header is never handled as a struct
 * image (02 §1 hard rule).  The decision order below is pinned by WB1's
 * reference implementation (tools/cluster-ref/refdec.c) and must stay in
 * lockstep with it -- tools/cluster-ref/check_c_side.py diffs this decoder
 * against every gold vector offline (02 §9).
 *
 * ASSUMPTION citations (tools/cluster-ref/README.md 假设登记簿):
 *   A-01 little-endian everywhere        A-03 payload_len counts payload only
 *   A-02 CRC covers header + payload     A-09 mtu budget subtracts 2 CRC bytes
 *   A-04 single-fragment frames are frag_flag_mismatch
 *   A-11 only RELIABLE frames may spend seq
 *   A-15 SEND txid must be zero (send-side rule enforced on receive)
 */
#include "cluster/frame.h"

/* 02 §2 payload column; -1 variable, -2 unknown.  Values from the table:
 * HELLO/HELLO_ACK 32 (§6), PING/PONG 8 (§2), CLOSE 0, ACK/NACK 4,
 * ERROR 8 (4B errno + 4B orig_txid), SEND/CALL/CALL_REPLY variable. */
int a20_frame_fixed_payload_len(uint8_t type)
{
    switch (type) {
    case A20_CLX_TYPE_HELLO:
    case A20_CLX_TYPE_HELLO_ACK:
        return 32;
    case A20_CLX_TYPE_PING:
    case A20_CLX_TYPE_PONG:
        return 8;
    case A20_CLX_TYPE_SEND:
    case A20_CLX_TYPE_CALL:
    case A20_CLX_TYPE_CALL_REPLY:
        return -1;
    case A20_CLX_TYPE_CLOSE:
        return 0;
    case A20_CLX_TYPE_ACK:
    case A20_CLX_TYPE_NACK:
        return 4;
    case A20_CLX_TYPE_ERROR:
        return 8;
    default:
        return -2;
    }
}

const char *a20_frame_verdict_str(int verdict)
{
    switch (verdict) {
    case A20_FRAME_OK:                          return "OK";
    case A20_FRAME_SHORT_FRAME:                 return "short_frame";
    case A20_FRAME_BAD_MAGIC:                   return "bad_magic";
    case A20_FRAME_UNSUPPORTED_VER:             return "unsupported_ver";
    case A20_FRAME_UNKNOWN_TYPE:                return "unknown_type";
    case A20_FRAME_UNSUPPORTED_CSUM_KIND:       return "unsupported_csum_kind";
    case A20_FRAME_PAYLOAD_OVER_MTU:            return "payload_len_over_mtu";
    case A20_FRAME_PAYLOAD_LEN_MISMATCH:        return "payload_len_mismatch";
    case A20_FRAME_CRC_MISMATCH:                return "crc_mismatch";
    case A20_FRAME_FRAG_FLAG_MISMATCH:          return "frag_flag_mismatch";
    case A20_FRAME_SEQ_ON_UNRELIABLE:           return "seq_on_unreliable_frame";
    case A20_FRAME_TXID_ON_SEND:                return "txid_on_send";
    case A20_FRAME_FIXED_PAYLOAD_LEN_MISMATCH:  return "fixed_payload_len_mismatch";
    default:                                    return "unknown_verdict";
    }
}

int a20_frame_encode(uint8_t *buf, uint32_t cap, uint32_t mtu,
                     const a20_frame_hdr_t *h, const uint8_t *payload)
{
    uint32_t crc_len, len, i;
    uint16_t crc;

    /* Empty-payload frames (02 §2: CLOSE always; any type whose payload_len
     * is 0) pass payload == NULL -- legal exactly when payload_len == 0.
     * Without this the 02-§8 CLOSE(dst_slot) could never be encoded. */
    if (!buf || !h || (!payload && h->payload_len))
        return -1;
    if (h->csum_kind != A20_CLX_CSUM_NONE && h->csum_kind != A20_CLX_CSUM_CCITT)
        return -1;
    /* 02 §1 sender rule: reserved bits are sent as zero.  COMPRESSED is a
     * reserved-to-zero flag in v0 (02 §3; A-16 governs receivers only). */
    if (h->flags & (uint16_t)~A20_CLX_FLAGS_KNOWN)
        return -1;
    if (h->flags & A20_CLX_FLAG_COMPRESSED)
        return -1;
    if (h->frag & A20_CLX_FRAG_RESERVED)
        return -1;
    if ((uint32_t)h->payload_len > a20_frame_max_payload(mtu))
        return -2;

    crc_len = (h->csum_kind == A20_CLX_CSUM_CCITT) ? A20_CLX_CRC_LEN : 0u;
    len = A20_CLX_HDR_LEN + (uint32_t)h->payload_len + crc_len;
    if (len > cap)
        return -3;

    a20_clx_put_le16(buf + A20_CLX_OFF_MAGIC, A20_CLX_MAGIC);
    buf[A20_CLX_OFF_VER] = h->ver;
    buf[A20_CLX_OFF_TYPE] = h->type;
    a20_clx_put_le16(buf + A20_CLX_OFF_FLAGS, h->flags);
    a20_clx_put_le16(buf + A20_CLX_OFF_FRAG, h->frag);
    a20_clx_put_le32(buf + A20_CLX_OFF_TXID, h->txid);
    a20_clx_put_le32(buf + A20_CLX_OFF_SRC_HASH, h->src_hash);
    a20_clx_put_le32(buf + A20_CLX_OFF_DST_HASH, h->dst_hash);
    a20_clx_put_le32(buf + A20_CLX_OFF_DST_SLOT, h->dst_slot);
    a20_clx_put_le32(buf + A20_CLX_OFF_SEQ, h->seq);
    a20_clx_put_le16(buf + A20_CLX_OFF_PAYLOAD_LEN, h->payload_len);
    buf[A20_CLX_OFF_TTL] = h->ttl;
    buf[A20_CLX_OFF_CSUM_KIND] = h->csum_kind;
    for (i = 0; i < (uint32_t)h->payload_len; i++)
        buf[A20_CLX_HDR_LEN + i] = payload[i];

    if (crc_len) {
        crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + (uint32_t)h->payload_len);
        a20_clx_put_le16(buf + A20_CLX_HDR_LEN + (uint32_t)h->payload_len, crc);
    }
    return (int)len;
}

a20_frame_verdict_t a20_frame_decode(const uint8_t *buf, uint32_t len,
                                     uint32_t mtu, a20_frame_hdr_t *out,
                                     const uint8_t **payload)
{
    uint32_t crc_len, want_len;
    uint16_t crc_rx, crc_calc;
    int fixed;

    if (payload)
        *payload = NULL;
    if (out)
        *out = (a20_frame_hdr_t){0};
    if (!buf || !out)
        return A20_FRAME_SHORT_FRAME;

    if (len < A20_CLX_HDR_LEN)
        return A20_FRAME_SHORT_FRAME;

    /* Fields are readable from here on for every later verdict (the
     * runner prints the header even for rejects past the length check). */
    out->ver         = buf[A20_CLX_OFF_VER];
    out->type        = buf[A20_CLX_OFF_TYPE];
    out->flags       = a20_clx_get_le16(buf + A20_CLX_OFF_FLAGS);
    out->frag        = a20_clx_get_le16(buf + A20_CLX_OFF_FRAG);
    out->txid        = a20_clx_get_le32(buf + A20_CLX_OFF_TXID);
    out->src_hash    = a20_clx_get_le32(buf + A20_CLX_OFF_SRC_HASH);
    out->dst_hash    = a20_clx_get_le32(buf + A20_CLX_OFF_DST_HASH);
    out->dst_slot    = a20_clx_get_le32(buf + A20_CLX_OFF_DST_SLOT);
    out->seq         = a20_clx_get_le32(buf + A20_CLX_OFF_SEQ);
    out->payload_len = a20_clx_get_le16(buf + A20_CLX_OFF_PAYLOAD_LEN);
    out->ttl         = buf[A20_CLX_OFF_TTL];
    out->csum_kind   = buf[A20_CLX_OFF_CSUM_KIND];

    if (a20_clx_get_le16(buf + A20_CLX_OFF_MAGIC) != A20_CLX_MAGIC)
        return A20_FRAME_BAD_MAGIC;
    /* 02 §1: version mismatch drops the frame; a higher ver is negotiated
     * in the HELLO phase, data frames are dropped silently. */
    if (out->ver != (uint8_t)A20_CLX_WIRE_VER)
        return A20_FRAME_UNSUPPORTED_VER;
    if (out->type < A20_CLX_TYPE_HELLO || out->type > A20_CLX_TYPE_ERROR)
        return A20_FRAME_UNKNOWN_TYPE;
    if (out->csum_kind != A20_CLX_CSUM_NONE &&
        out->csum_kind != A20_CLX_CSUM_CCITT)
        return A20_FRAME_UNSUPPORTED_CSUM_KIND;

    /* 02 §1 + A-09: the two CRC bytes are subtracted unconditionally. */
    if (mtu < A20_CLX_HDR_LEN + A20_CLX_CRC_LEN ||
        (uint32_t)out->payload_len > a20_frame_max_payload(mtu))
        return A20_FRAME_PAYLOAD_OVER_MTU;

    out->crc_present = (out->csum_kind == A20_CLX_CSUM_CCITT);
    crc_len = out->crc_present ? A20_CLX_CRC_LEN : 0u;

    /* 02 §1 + A-03: payload_len must match what the transport received. */
    want_len = A20_CLX_HDR_LEN + (uint32_t)out->payload_len + crc_len;
    if (len != want_len)
        return A20_FRAME_PAYLOAD_LEN_MISMATCH;

    if (payload)
        *payload = buf + A20_CLX_HDR_LEN;

    if (out->crc_present) {
        crc_rx = a20_clx_get_le16(buf + A20_CLX_HDR_LEN +
                                  (uint32_t)out->payload_len);
        crc_calc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN +
                                               (uint32_t)out->payload_len);
        out->crc_value = crc_rx;
        if (crc_rx != crc_calc)
            return A20_FRAME_CRC_MISMATCH;
    }

    /* 02 §3/§5 + A-04: the FRAGMENTED flag and the frag field must agree;
     * a message that fits one frame is not fragmented (frag=0|last is the
     * single-frame violation the mal-frag-* vectors pin). */
    if (out->flags & A20_CLX_FLAG_FRAGMENTED) {
        if ((out->frag & A20_CLX_FRAG_SEQ_MASK) == 0 &&
            (out->frag & A20_CLX_FRAG_LAST))
            return A20_FRAME_FRAG_FLAG_MISMATCH;
    } else if (out->frag != 0) {
        return A20_FRAME_FRAG_FLAG_MISMATCH;
    }

    /* 02 §1 + A-11: seq is the reliable-tier number. */
    if (!(out->flags & A20_CLX_FLAG_RELIABLE) && out->seq != 0)
        return A20_FRAME_SEQ_ON_UNRELIABLE;
    /* 02 §1 + A-15: SEND carries txid 0. */
    if (out->type == A20_CLX_TYPE_SEND && out->txid != 0)
        return A20_FRAME_TXID_ON_SEND;

    fixed = a20_frame_fixed_payload_len(out->type);
    if (fixed >= 0 && (uint32_t)fixed != (uint32_t)out->payload_len)
        return A20_FRAME_FIXED_PAYLOAD_LEN_MISMATCH;

    return A20_FRAME_OK;
}
