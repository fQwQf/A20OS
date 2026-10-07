#ifndef _CLUSTER_FRAME_H
#define _CLUSTER_FRAME_H

/*
 * Cluster wire-frame codec -- the canonical kernel-side frame encoder and
 * layer-1 decoder (docs/cluster/02-wire-protocol.md §1-§2, WA1 track).
 *
 * Ownership and seams (all registered in the docs, 2026-10):
 *   - This header owns the frame PUBLIC interface: field-by-field encode and
 *     verdict-classified decode, pure functions, no locks, no allocation,
 *     every buffer caller-owned (docs/cluster/03-kernel-impl.md §2 frame.c
 *     row).  WC1's uart.c/mcu leaf need only these signatures; anything they
 *     miss lands here, not in a second codec.
 *   - Byte-order accessors, CRC16-CCITT and FNV-1a are the shared header-only
 *     primitives of kernel/cluster/uart.h (04-transports.md §4 实现期记录
 *     note 4: the primitive layer lives in uart.h so head node and leaf emit
 *     bit-identical frames).  This file reuses them and never redefines one.
 *   - The layer-1 verdicts and their order are pinned by WB1's reference
 *     decoder (tools/cluster-ref/refdec.c) and the gold vectors in
 *     tools/cluster-ref/vectors/; a20_frame_verdict_str() returns the exact
 *     clframe.py strings so tools/cluster-ref/check_c_side.py can diff this
 *     decoder against every vector offline.
 *
 * Spec basis, quoted by section:
 *   02-wire-protocol.md §1  layout, little-endian, CRC over header+payload,
 *                           payload_len vs received length, reserved bits.
 *   02-wire-protocol.md §2  message types and their fixed payload sizes.
 *   02-wire-protocol.md §3  flags; bit layout of the frag field is §1/§5.
 *   01-abi.md §节点哈希      FNV-1a node hash (primitive from uart.h).
 *   tools/cluster-ref/README.md 假设登记簿 A-01..A-09, A-11..A-13, A-15.
 */

#include "core/types.h"
#include "cluster/uart.h"

/* ------------------------------------------------------------------ */
/* frag field (02 §1: low 12 bits sequence, bit15 last, bit12-14 res.) */
/* ------------------------------------------------------------------ */

#define A20_CLX_FRAG_SEQ_MASK  0x0FFFu
#define A20_CLX_FRAG_LAST      0x8000u
#define A20_CLX_FRAG_RESERVED  0x7000u

/* Known flag set (02 §3).  Bits 4..15 are reserved: senders zero them,
 * receivers ignore them (A-16 for COMPRESSED). */
#define A20_CLX_FLAGS_KNOWN    0x000Fu

/* ------------------------------------------------------------------ */
/* Layer-1 verdicts.  The enum order mirrors the decision order of      */
/* clframe.decode_frame()/refdec.c; the strings are the exact R_*       */
/* constants of clframe.py so the gold-vector runner can diff them.     */
/* ------------------------------------------------------------------ */

typedef enum a20_frame_verdict {
    A20_FRAME_OK                        = 0,
    A20_FRAME_SHORT_FRAME               = 1,  /* short_frame               */
    A20_FRAME_BAD_MAGIC                 = 2,  /* bad_magic                 */
    A20_FRAME_UNSUPPORTED_VER           = 3,  /* unsupported_ver           */
    A20_FRAME_UNKNOWN_TYPE              = 4,  /* unknown_type              */
    A20_FRAME_UNSUPPORTED_CSUM_KIND     = 5,  /* unsupported_csum_kind     */
    A20_FRAME_PAYLOAD_OVER_MTU          = 6,  /* payload_len_over_mtu      */
    A20_FRAME_PAYLOAD_LEN_MISMATCH      = 7,  /* payload_len_mismatch      */
    A20_FRAME_CRC_MISMATCH              = 8,  /* crc_mismatch              */
    A20_FRAME_FRAG_FLAG_MISMATCH        = 9,  /* frag_flag_mismatch        */
    A20_FRAME_SEQ_ON_UNRELIABLE         = 10, /* seq_on_unreliable_frame   */
    A20_FRAME_TXID_ON_SEND              = 11, /* txid_on_send              */
    A20_FRAME_FIXED_PAYLOAD_LEN_MISMATCH = 12 /* fixed_payload_len_mismatch */
} a20_frame_verdict_t;

const char *a20_frame_verdict_str(int verdict);

/* ------------------------------------------------------------------ */
/* Decoded header view.  NEVER serialized as a memory image (02 §1):   */
/* encode/decode go field by field; this struct is only the in-memory  */
/* argument/result carrier.                                            */
/* ------------------------------------------------------------------ */

typedef struct a20_frame_hdr {
    uint8_t  ver;
    uint8_t  type;
    uint16_t flags;
    uint16_t frag;        /* raw field: seq | last | reserved            */
    uint32_t txid;
    uint32_t src_hash;
    uint32_t dst_hash;
    uint32_t dst_slot;
    uint32_t seq;
    uint16_t payload_len;
    uint8_t  ttl;
    uint8_t  csum_kind;
    uint8_t  crc_present; /* 1 when csum_kind == A20_CLX_CSUM_CCITT      */
    uint16_t crc_value;   /* received CRC (valid when crc_present)       */
} a20_frame_hdr_t;

/* 02 §1 via A-09: payload_len <= mtu - 32 - 2, the 2 CRC bytes are
 * subtracted unconditionally, also when csum_kind=0. */
static inline uint32_t a20_frame_max_payload(uint32_t mtu)
{
    return (mtu >= A20_CLX_HDR_LEN + A20_CLX_CRC_LEN)
               ? mtu - A20_CLX_HDR_LEN - A20_CLX_CRC_LEN
               : 0;
}

/* 02 §2 payload column: -1 variable, -2 unknown type. */
int a20_frame_fixed_payload_len(uint8_t type);

/*
 * Encode one frame into buf (caller-owned, 02 §1).  Sets magic/ver and
 * every field explicitly, appends payload, attaches CRC16 when
 * h->csum_kind == A20_CLX_CSUM_CCITT (A-02: CRC covers header + payload).
 *
 * Sender-side hard rules enforced here (02 §1 "必须发零"):
 *   - flags reserved bits (4..15) and COMPRESSED (A-16 receive side only)
 *     must be zero;
 *   - frag reserved bits (12..14) must be zero;
 *   - payload_len <= a20_frame_max_payload(mtu).
 *
 * Returns the frame length (32 + payload_len + crc), or:
 *   -1  bad argument (NULL, reserved bits set, unknown csum_kind)
 *   -2  payload_len over mtu
 *   -3  cap too small
 */
int a20_frame_encode(uint8_t *buf, uint32_t cap, uint32_t mtu,
                     const a20_frame_hdr_t *h, const uint8_t *payload);

/*
 * Layer-1 decode, same contract and decision order as
 * clframe.decode_frame()/refdec.c (02 §9 offline vectors):
 *
 *   buf/len   received bytes; len is what the transport actually got.
 *   mtu       RECEIVING transport's MTU (bounds payload_len, 02 §1).
 *   out       filled as far as the verdict allows: fields are read after
 *             the short-frame check, payload bounds are known after the
 *             length check.
 *   payload   set to buf+32 when the length check passed (verdict worse
 *             than A20_FRAME_PAYLOAD_LEN_MISMATCH keeps it NULL; the
 *             earlier rejects may still carry a readable payload, but the
 *             caller only ever needs it for accepted frames).
 *
 * Returns A20_FRAME_OK (accept) or the reject reason; out->crc_value is
 * the RECEIVED checksum so the runner can print it.  Layer-2 policy (TTL
 * exhausted, destination not local, duplicated seq, HELLO negotiation) is
 * NOT part of this decoder -- 02 §1 "接收方对非零保留位不拒绝" and §10
 * place those in the dispatch layer with rx_drops accounting.
 */
a20_frame_verdict_t a20_frame_decode(const uint8_t *buf, uint32_t len,
                                     uint32_t mtu, a20_frame_hdr_t *out,
                                     const uint8_t **payload);

#endif /* _CLUSTER_FRAME_H */
