#ifndef _CLUSTER_UART_H
#define _CLUSTER_UART_H

/*
 * Cluster UART transport -- shared wire codec and head-node link API.
 *
 * Ownership: WC1 track (docs/cluster/impl-prompts.md 文件所有权表: WC owns
 * kernel/cluster/uart.c and kernel/mcu/).  This header is the one shared
 * seam between the two halves of that ownership: the head-node transport in
 * kernel/cluster/uart.c and the MCU leaf protocol face in kernel/mcu/leaf.c
 * both speak the same wire format through the header-only codec below.
 * Header-only on purpose (same pattern as drivers/gpu/drm_geom.h): the codec
 * must be bit-identical on both sides and host-testable from tools/tests/
 * without linking any kernel object.
 *
 * Spec basis, quoted by section:
 *   docs/cluster/02-wire-protocol.md  -- frame layout (§1), types (§2),
 *                                        flags (§3), HELLO payload (§6),
 *                                        counters (§10).
 *   docs/cluster/04-transports.md     -- UART transport (§4): SLIP variant,
 *                                        MTU 256, 50 ms inter-byte timeout,
 *                                        2-byte short address.
 *   docs/cluster/01-abi.md            -- node hash (FNV-1a 32), caps, errno
 *                                        values carried in ERROR frames.
 *   docs/cluster/03-kernel-impl.md    -- §4 tiering (CLUSTER_PROFILE), §2
 *                                        "pure functions, no locks, no
 *                                        allocation, caller supplies the
 *                                        buffer" for the codec half.
 *
 * The SLIP golden vectors from WB1 (tools/cluster-ref/) were still being
 * produced when this file was written (2026-10); the codec implements the
 * 04-§4 prose exactly and cites the section per rule.  The vector cross-check
 * happens at the integration step; where the two disagree, the prose wins
 * (impl-prompts.md 故障处置表).
 */

#include "core/types.h"

/* ------------------------------------------------------------------ */
/* Compile-time tier (03-kernel-impl.md §4).                           */
/*                                                                     */
/* The Makefile wiring (-DCONFIG_CLUSTER_PROFILE=$(CLUSTER_PROFILE),   */
/* NET_PROFILE style) belongs to WA3 and may not exist yet; until it   */
/* lands the tier is derived the way §4 says the profile system does   */
/* anyway: PROFILE=mcu implies CLUSTER_PROFILE=1, anything else is the */
/* DEFAULT tier 2.  An explicit -DCONFIG_CLUSTER_PROFILE always wins.  */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_CLUSTER_PROFILE
#ifdef CONFIG_MCU
#define CONFIG_CLUSTER_PROFILE 1
#else
#define CONFIG_CLUSTER_PROFILE 2
#endif
#endif

#define A20_CLX_PROFILE_MCU     1
#define A20_CLX_PROFILE_DEFAULT 2
#define A20_CLX_PROFILE_SERVER  3

/* ------------------------------------------------------------------ */
/* Frame layout, 02-wire-protocol.md §1.  All multi-byte fields        */
/* little-endian; encode/decode goes through the explicit accessors    */
/* below, never through a struct memory image (02 §1 hard rule).       */
/* ------------------------------------------------------------------ */

#define A20_CLX_HDR_LEN        32u
#define A20_CLX_CRC_LEN        2u
#define A20_CLX_MAGIC          0x4C43u /* "CL", LE on the wire: 'C' then 'L' */
#define A20_CLX_WIRE_VER       0u
#define A20_CLX_TTL_DEFAULT    8u
#define A20_CLX_CSUM_NONE      0u
#define A20_CLX_CSUM_CCITT     1u

/* Field offsets into the 32-byte header. */
#define A20_CLX_OFF_MAGIC       0u  /* u16 */
#define A20_CLX_OFF_VER         2u  /* u8  */
#define A20_CLX_OFF_TYPE        3u  /* u8  */
#define A20_CLX_OFF_FLAGS       4u  /* u16 */
#define A20_CLX_OFF_FRAG        6u  /* u16 */
#define A20_CLX_OFF_TXID        8u  /* u32 */
#define A20_CLX_OFF_SRC_HASH   12u  /* u32 */
#define A20_CLX_OFF_DST_HASH   16u  /* u32 */
#define A20_CLX_OFF_DST_SLOT   20u  /* u32 */
#define A20_CLX_OFF_SEQ        24u  /* u32 */
#define A20_CLX_OFF_PAYLOAD_LEN 28u /* u16 */
#define A20_CLX_OFF_TTL        30u  /* u8  */
#define A20_CLX_OFF_CSUM_KIND  31u  /* u8  */
#define A20_CLX_OFF_PAYLOAD    32u

/* Message types, 02 §2. */
#define A20_CLX_TYPE_HELLO      1u
#define A20_CLX_TYPE_HELLO_ACK  2u
#define A20_CLX_TYPE_PING       3u
#define A20_CLX_TYPE_PONG       4u
#define A20_CLX_TYPE_SEND       5u
#define A20_CLX_TYPE_CALL       6u
#define A20_CLX_TYPE_CALL_REPLY 7u
#define A20_CLX_TYPE_CLOSE      8u
#define A20_CLX_TYPE_ACK        9u
#define A20_CLX_TYPE_NACK      10u
#define A20_CLX_TYPE_ERROR     11u

/* Flags, 02 §3.  Bits 4..15 are reserved: send zero, ignore on receive. */
#define A20_CLX_FLAG_RELIABLE   (1u << 0)
#define A20_CLX_FLAG_FRAGMENTED (1u << 1)
#define A20_CLX_FLAG_BROADCAST  (1u << 2)
#define A20_CLX_FLAG_COMPRESSED (1u << 3) /* v0: reserved, must be 0 */

/* HELLO payload, 02 §6: fixed 32 bytes. */
#define A20_CLX_HELLO_LEN            32u
#define A20_CLX_HELLO_OFF_NODE_ID     0u  /* 16 bytes */
#define A20_CLX_HELLO_OFF_PROTO_MIN  16u
#define A20_CLX_HELLO_OFF_PROTO_MAX  17u
#define A20_CLX_HELLO_OFF_TIER       18u
#define A20_CLX_HELLO_OFF_CAPS       19u
#define A20_CLX_HELLO_OFF_SHORT_ADDR 20u /* u16 LE */
#define A20_CLX_HELLO_OFF_LINK_ADDR_LEN 22u
#define A20_CLX_HELLO_OFF_RESERVED   23u
#define A20_CLX_HELLO_OFF_NONCE      24u /* 8 opaque bytes */
#define A20_CLX_NODE_ID_LEN          16u

/* Capability bits, 01-abi §能力位. */
#define A20_CLX_CAP_RELAY    (1u << 0)
#define A20_CLX_CAP_RELIABLE (1u << 1)
#define A20_CLX_CAP_LEAF     (1u << 2)

/* Profile tier values carried in the HELLO payload (01-abi/03-§4). */
#define A20_CLX_TIER_MCU     1u
#define A20_CLX_TIER_DEFAULT 2u
#define A20_CLX_TIER_SERVER  3u

/*
 * errno values carried in the ERROR frame payload (02 §2: 4B errno + 4B
 * orig_txid; 02 §8: only cluster errnos).  Values are the kernel's
 * kernel/include/ipc/ipc.h numbers, verified 2026-10:
 * NOT_FOUND 24, NODE_UNREACHABLE 26, CLUSTER_TIMEOUT 27, REMOTE_CLOSED 28,
 * CLUSTER_UNSUPPORTED 29.  An unknown code decodes as CLUSTER_UNSUPPORTED.
 */
#define A20_CLX_WERR_NOT_FOUND           24u
#define A20_CLX_WERR_NODE_UNREACHABLE    26u
#define A20_CLX_WERR_CLUSTER_TIMEOUT     27u
#define A20_CLX_WERR_REMOTE_CLOSED       28u
#define A20_CLX_WERR_CLUSTER_UNSUPPORTED 29u

/* Per-link counters, 02 §10.  Append-only: new counters go at the tail. */
typedef struct a20_clx_link_counters {
    uint64_t tx_frames;
    uint64_t rx_frames;
    uint64_t tx_drops;
    uint64_t rx_drops;
    uint64_t rx_malformed;
    uint64_t retransmits;
    uint64_t reasm_timeouts;
    uint64_t reasm_evicted;
    uint64_t dedup_drops;
    uint64_t hello_rejects;
} a20_clx_link_counters_t;

/* ------------------------------------------------------------------ */
/* UART transport constants, 04-transports.md §4.                      */
/* ------------------------------------------------------------------ */

#define A20_CLX_UART_TRANSPORT_ID     2u
#define A20_CLX_UART_MTU              256u /* header + payload + CRC, no SLIP */
#define A20_CLX_UART_MAX_PAYLOAD \
    (A20_CLX_UART_MTU - A20_CLX_HDR_LEN - A20_CLX_CRC_LEN) /* 222 */
#define A20_CLX_UART_SHORT_UNASSIGNED 0xFFFFu

/* SLIP variant, 04 §4 table: END delimits head and tail, ESC prefixes. */
#define A20_CLX_SLIP_END    0xC0u
#define A20_CLX_SLIP_ESC    0xDBu
#define A20_CLX_SLIP_ESC_END 0xDCu
#define A20_CLX_SLIP_ESC_ESC 0xDDu

/* 04 §4: "字节间静默 > 50ms 丢弃当前收半帧，回到找 END 状态". */
#define A20_CLX_UART_INTERBYTE_MS 50u
/* 04 §5 table: UART heartbeat 5 s, head -> leaf. */
#define A20_CLX_UART_PING_PERIOD_MS 5000u
/* 04 §4 leaf: "50s 无 PING 回 DISCONNECTED，短地址作废". */
#define A20_CLX_UART_LEAF_IDLE_MS 50000u

/*
 * HELLO dial timing, 02 §6: "HELLO_SENT --超时×3--> DOWN".  The section
 * fixes the retry count but not the per-attempt timeout; 2 s is four full
 * frames at 115200 8N1 (04 §4 sizes a full frame at ~30 ms) and is an
 * implementation value inside the spec's shape, recorded here so the
 * integration step can retune it in one place.
 */
#define A20_CLX_UART_HELLO_TIMEOUT_MS 2000u
#define A20_CLX_UART_HELLO_RETRIES    3u

/*
 * 04 §4 头节点侧: "一 UART 一叶子，多叶子用多 UART" -- so the head-side
 * link table is small and static.  Four covers every board this tree boots
 * with a spare serial port today; raising it costs 512 B of BSS per link.
 */
#define A20_CLX_UART_MAX_LINKS 4

/* SLIP worst case: every byte escapes to two, plus head and tail END. */
#define A20_CLX_SLIP_ENCODED_LEN(n) ((n) * 2u + 2u)

/* ------------------------------------------------------------------ */
/* Explicit little-endian field accessors (02 §1: no struct images).   */
/* ------------------------------------------------------------------ */

static inline uint16_t a20_clx_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t a20_clx_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t a20_clx_get_le64(const uint8_t *p)
{
    return (uint64_t)a20_clx_get_le32(p) |
           ((uint64_t)a20_clx_get_le32(p + 4) << 32);
}

static inline void a20_clx_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void a20_clx_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void a20_clx_put_le64(uint8_t *p, uint64_t v)
{
    a20_clx_put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    a20_clx_put_le32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

/* ------------------------------------------------------------------ */
/* Codec primitives.  Pure functions: no locks, no allocation, the     */
/* caller owns every buffer (03-kernel-impl.md §2 frame.c row).        */
/* ------------------------------------------------------------------ */

/*
 * 02 §1 csum_kind=1: CRC16-CCITT, poly 0x1021, init 0xFFFF, no reflection,
 * no final xor -- CRC-16/CCITT-FALSE.  Catalogue check value: CRC of
 * "123456789" is 0x29B1 (asserted in tools/tests/test_clx_uart.c so the
 * parameterisation cannot silently drift).
 */
static inline uint16_t a20_clx_crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000u)
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/*
 * 01-abi §节点哈希: FNV-1a 32 over the 16 node-id bytes, offset basis
 * 2166136261, prime 16777619.  Anchors (same ones tools/cluster-ref
 * asserts): all-zero -> 0x69691905, all-0xff -> 0x360779f5.
 */
static inline uint32_t a20_clx_fnv1a32(const uint8_t *id16)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < 16; i++) {
        h ^= (uint32_t)id16[i];
        h *= 16777619u;
    }
    return h;
}

/*
 * Build one complete CL frame (header + payload + CRC16, 02 §1) into buf,
 * field by field, never as a struct image.  UART forces csum_kind=1
 * (04 §4), so the builder always attaches the CRC.  Shared by the head
 * transport (kernel/cluster/uart.c) and the MCU leaf (kernel/mcu/leaf.c)
 * so both ends emit byte-identical frames.
 *
 * Returns the frame length (32 + payload_len + 2), or -1 when it does not
 * fit cap or the payload exceeds the UART single-frame budget.
 */
static inline int a20_clx_build_frame(uint8_t *buf, uint32_t cap,
                                      uint8_t type, uint16_t flags,
                                      uint16_t frag, uint32_t txid,
                                      uint32_t src_hash, uint32_t dst_hash,
                                      uint32_t dst_slot, const uint8_t *payload,
                                      uint16_t payload_len)
{
    uint32_t len = A20_CLX_HDR_LEN + (uint32_t)payload_len + A20_CLX_CRC_LEN;
    uint16_t crc;
    uint32_t i;

    if (len > cap || payload_len > A20_CLX_UART_MAX_PAYLOAD)
        return -1;

    a20_clx_put_le16(buf + A20_CLX_OFF_MAGIC, A20_CLX_MAGIC);
    buf[A20_CLX_OFF_VER] = (uint8_t)A20_CLX_WIRE_VER;
    buf[A20_CLX_OFF_TYPE] = type;
    a20_clx_put_le16(buf + A20_CLX_OFF_FLAGS, flags);
    a20_clx_put_le16(buf + A20_CLX_OFF_FRAG, frag);
    a20_clx_put_le32(buf + A20_CLX_OFF_TXID, txid);
    a20_clx_put_le32(buf + A20_CLX_OFF_SRC_HASH, src_hash);
    a20_clx_put_le32(buf + A20_CLX_OFF_DST_HASH, dst_hash);
    a20_clx_put_le32(buf + A20_CLX_OFF_DST_SLOT, dst_slot);
    a20_clx_put_le32(buf + A20_CLX_OFF_SEQ, 0); /* UART v0: no RELIABLE */
    a20_clx_put_le16(buf + A20_CLX_OFF_PAYLOAD_LEN, payload_len);
    buf[A20_CLX_OFF_TTL] = (uint8_t)A20_CLX_TTL_DEFAULT;
    buf[A20_CLX_OFF_CSUM_KIND] = (uint8_t)A20_CLX_CSUM_CCITT;
    for (i = 0; i < (uint32_t)payload_len; i++)
        buf[A20_CLX_HDR_LEN + i] = payload[i];
    crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + (uint32_t)payload_len);
    a20_clx_put_le16(buf + A20_CLX_HDR_LEN + (uint32_t)payload_len, crc);
    return (int)len;
}

/*
 * 02 §6 HELLO payload: fixed 32 bytes, little-endian short_addr, opaque
 * 8-byte nonce.  Shared by both ends for the same reason as the builder.
 */
static inline void a20_clx_build_hello_payload(const uint8_t *node_id16,
                                               uint8_t proto_min,
                                               uint8_t proto_max, uint8_t tier,
                                               uint8_t caps,
                                               uint16_t short_addr,
                                               uint64_t nonce, uint8_t *out32)
{
    int i;

    for (i = 0; i < 16; i++)
        out32[A20_CLX_HELLO_OFF_NODE_ID + (uint32_t)i] = node_id16[i];
    out32[A20_CLX_HELLO_OFF_PROTO_MIN] = proto_min;
    out32[A20_CLX_HELLO_OFF_PROTO_MAX] = proto_max;
    out32[A20_CLX_HELLO_OFF_TIER] = tier;
    out32[A20_CLX_HELLO_OFF_CAPS] = caps;
    a20_clx_put_le16(out32 + A20_CLX_HELLO_OFF_SHORT_ADDR, short_addr);
    out32[A20_CLX_HELLO_OFF_LINK_ADDR_LEN] = 2; /* uart short addr, 04 §4 */
    out32[A20_CLX_HELLO_OFF_RESERVED] = 0;
    a20_clx_put_le64(out32 + A20_CLX_HELLO_OFF_NONCE, nonce);
}

/*
 * 04 §4 SLIP variant encoder: one END at the head, one at the tail, 0xC0
 * inside the frame becomes DB DC, 0xDB becomes DB DD.  The head END doubles
 * as the receiver's re-sync point.  Returns the escaped length written, or
 * -1 when out_cap is too small (worst case is A20_CLX_SLIP_ENCODED_LEN(len)).
 */
static inline int a20_clx_slip_encode(const uint8_t *frame, uint32_t len,
                                      uint8_t *out, uint32_t out_cap)
{
    uint32_t need = A20_CLX_SLIP_ENCODED_LEN(len);
    uint32_t o = 0, i;

    if (need > out_cap)
        return -1;
    out[o++] = A20_CLX_SLIP_END;
    for (i = 0; i < len; i++) {
        uint8_t b = frame[i];
        if (b == A20_CLX_SLIP_END) {
            out[o++] = A20_CLX_SLIP_ESC;
            out[o++] = A20_CLX_SLIP_ESC_END;
        } else if (b == A20_CLX_SLIP_ESC) {
            out[o++] = A20_CLX_SLIP_ESC;
            out[o++] = A20_CLX_SLIP_ESC_ESC;
        } else {
            out[o++] = b;
        }
    }
    out[o++] = A20_CLX_SLIP_END;
    return (int)o;
}

/*
 * Streaming SLIP decoder, fed one UART byte at a time.  The caller owns the
 * decode buffer (04 §4: "单接收缓冲 512B 静态数组" on the leaf side).
 *
 * Semantics follow 04 §4 as settled against the gold vectors in
 * tools/cluster-ref/vectors/slip/ (2026-10):
 *   - the head END arms the delimiter and doubles as the re-sync point;
 *     inter-frame noise before it is ignored;
 *   - END closes the frame and delivers it (empty frame = resync artifact,
 *     no counter event);
 *   - ESC consumes the next byte unconditionally: DB DC -> data 0xC0, DB DD
 *     -> data 0xDB, anything else (END included) is a bad pair that is
 *     consumed without producing a data byte while the half frame stays
 *     alive (slip-mal-badescape-01);
 *   - a frame that no longer fits the buffer is discarded IMMEDIATELY
 *     (received bytes voided, -1 reported once) and the decoder swallows to
 *     the next END to re-sync (slip-mal-oversize-05, 04 §4 "即刻丢弃");
 *   - the 50 ms inter-byte silence rule runs in
 *     a20_clx_slip_rx_timeout().
 *
 * Return codes:
 *   1  a complete frame is sitting in ->buf with ->len bytes (the frame
 *      stays readable there until the next head END)
 *   0  byte consumed, decoder still hunting or accumulating
 *  -1  the half frame in progress was discarded (over cap); reported once,
 *      then the decoder swallows to the next END and re-syncs
 *
 * now_ticks timestamps every byte so the timeout can enforce 04 §4 without
 * the codec depending on the kernel timer (keeps this half host-testable).
 */
typedef struct a20_clx_slip_rx {
    uint8_t *buf;                 /* caller-owned decode buffer          */
    uint32_t cap;                 /* size of ->buf                       */
    uint32_t len;                 /* bytes decoded into ->buf            */
    uint64_t interbyte_ticks;     /* silence budget in timer ticks       */
    uint64_t last_byte_ticks;     /* tick of the most recently fed byte  */
    uint8_t in_frame;             /* saw a head END, accumulating        */
    uint8_t in_escape;            /* previous byte was ESC               */
    uint8_t overflow;             /* current frame exceeded ->cap        */
} a20_clx_slip_rx_t;

static inline void a20_clx_slip_rx_init(a20_clx_slip_rx_t *rx, uint8_t *buf,
                                        uint32_t cap,
                                        uint64_t interbyte_ticks)
{
    rx->buf = buf;
    rx->cap = cap;
    rx->len = 0;
    rx->interbyte_ticks = interbyte_ticks;
    rx->last_byte_ticks = 0;
    rx->in_frame = 0;
    rx->in_escape = 0;
    rx->overflow = 0;
}

static inline void a20_clx_slip_rx_reset(a20_clx_slip_rx_t *rx)
{
    rx->len = 0;
    rx->in_frame = 0;
    rx->in_escape = 0;
    rx->overflow = 0;
}

static inline int a20_clx_slip_rx_byte(a20_clx_slip_rx_t *rx, uint8_t b,
                                       uint64_t now_ticks)
{
    rx->last_byte_ticks = now_ticks;

    /* An open escape consumes the next byte unconditionally -- a raw END
     * does not close the frame while an escape is open (only DB DC and
     * DB DD are defined, 04 §4; other pairs are consumed without
     * producing a data byte and the half frame stays alive). */
    if (rx->in_escape) {
        rx->in_escape = 0;
        if (b == A20_CLX_SLIP_ESC_END)
            b = A20_CLX_SLIP_END;
        else if (b == A20_CLX_SLIP_ESC_ESC)
            b = A20_CLX_SLIP_ESC;
        else
            return 0; /* bad escape pair: consumed, frame continues */
        if (rx->overflow || !rx->in_frame)
            return 0;
        if (rx->len >= rx->cap) {
            rx->overflow = 1;
            return -1; /* 04 §4: discard immediately, bytes voided */
        }
        rx->buf[rx->len++] = b;
        return 0;
    }

    if (b == A20_CLX_SLIP_END) {
        if (!rx->in_frame) {
            /* Head END: arms the delimiter and doubles as the re-sync
             * point (04 §4).  Nothing to deliver yet. */
            rx->in_frame = 1;
            rx->len = 0;
            rx->overflow = 0;
            return 0;
        }
        if (rx->overflow) {
            /* Already reported at the overflowing byte; the closing END
             * just re-syncs the stream. */
            a20_clx_slip_rx_reset(rx);
            return 0;
        }
        if (rx->len == 0) {
            /* END directly after a head END: an empty frame is the
             * re-sync artifact, not a frame and not a counter event. */
            return 0;
        }
        rx->in_frame = 0;
        return 1; /* complete frame in ->buf, ->len valid */
    }

    if (!rx->in_frame)
        return 0; /* inter-frame noise: ignored between ENDs */

    if (b == A20_CLX_SLIP_ESC) {
        rx->in_escape = 1;
        return 0;
    }
    if (rx->overflow)
        return 0;
    if (rx->len >= rx->cap) {
        rx->overflow = 1;
        return -1; /* 04 §4: discard immediately, bytes voided */
    }
    rx->buf[rx->len++] = b;
    return 0;
}

/*
 * 04 §4 partial-frame timeout: more than interbyte_ticks of silence since
 * the last byte while a frame is open drops the half frame and returns to
 * hunting END.  Returns 1 when a half frame was dropped (caller counts it,
 * rx_drops), 0 otherwise.
 */
static inline int a20_clx_slip_rx_timeout(a20_clx_slip_rx_t *rx,
                                          uint64_t now_ticks)
{
    if (!rx->in_frame)
        return 0;
    if (now_ticks - rx->last_byte_ticks <= rx->interbyte_ticks)
        return 0;
    a20_clx_slip_rx_reset(rx);
    return 1;
}

/*
 * Transport-level frame screen: the 02 §1 hard rules whose judgement needs
 * exactly the length the transport received.  Structural violations report
 * MALFORMED (02 §10 rx_malformed); a well-formed frame that must still be
 * refused reports DROP (rx_drops).  Frame version and unknown types are
 * dispatch decisions left to the caller.
 *
 * Verdict constants:
 *   OK        frame may proceed to dispatch
 *   MALFORMED magic / csum_kind / payload_len-vs-received-length / CRC /
 *             length bounds violation (02 §1 hard rules, 04 §4 csum_kind=1)
 *   DROP      well-formed but refused: TTL exhausted in transit (02 §1)
 */
#define A20_CLX_SCREEN_OK        0
#define A20_CLX_SCREEN_MALFORMED 1
#define A20_CLX_SCREEN_DROP      2

static inline int a20_clx_screen_frame(const uint8_t *frame, uint32_t len)
{
    uint16_t magic, payload_len, crc_rx, crc_calc;

    if (len < A20_CLX_HDR_LEN + A20_CLX_CRC_LEN || len > A20_CLX_UART_MTU)
        return A20_CLX_SCREEN_MALFORMED;
    magic = a20_clx_get_le16(frame + A20_CLX_OFF_MAGIC);
    if (magic != A20_CLX_MAGIC)
        return A20_CLX_SCREEN_MALFORMED;
    /* 04 §4: "csum_kind=1 强制" on the UART transport. */
    if (frame[A20_CLX_OFF_CSUM_KIND] != (uint8_t)A20_CLX_CSUM_CCITT)
        return A20_CLX_SCREEN_MALFORMED;
    /* 02 §1: "payload_len 与传输层实收长度不符 -> 帧丢弃 + rx_malformed". */
    payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    if ((uint32_t)payload_len != len - A20_CLX_HDR_LEN - A20_CLX_CRC_LEN ||
        payload_len > A20_CLX_UART_MAX_PAYLOAD)
        return A20_CLX_SCREEN_MALFORMED;
    if (frame[A20_CLX_OFF_TTL] == 0)
        return A20_CLX_SCREEN_DROP;
    crc_rx = a20_clx_get_le16(frame + A20_CLX_HDR_LEN + (uint32_t)payload_len);
    crc_calc =
        a20_clx_crc16_ccitt(frame, A20_CLX_HDR_LEN + (uint32_t)payload_len);
    if (crc_rx != crc_calc)
        return A20_CLX_SCREEN_MALFORMED;
    return A20_CLX_SCREEN_OK;
}

/*
 * SLIP TX escape, 04 §4 mapping (0xC0 -> DB DC, 0xDB -> DB DD), shared by
 * both ends: sink writes one raw wire byte and returns 0 or -errno.
 */
static inline int a20_clx_slip_emit(uint8_t b, int (*sink)(void *, uint8_t),
                                    void *ctx)
{
    int rc;

    if (b == A20_CLX_SLIP_END) {
        rc = sink(ctx, A20_CLX_SLIP_ESC);
        if (rc != 0)
            return rc;
        return sink(ctx, A20_CLX_SLIP_ESC_END);
    }
    if (b == A20_CLX_SLIP_ESC) {
        rc = sink(ctx, A20_CLX_SLIP_ESC);
        if (rc != 0)
            return rc;
        return sink(ctx, A20_CLX_SLIP_ESC_ESC);
    }
    return sink(ctx, b);
}

/* ------------------------------------------------------------------ */
/* Head-node transport API (kernel/cluster/uart.c).  This is the seam  */
/* WA's kernel/cluster/transport.c wraps into the a20_clx_transport_t  */
/* contract of 04 §1: send -> a20_clx_uart_send_frame(), poll ->       */
/* a20_clx_uart_poll_all(), rx/link events -> the handlers below.      */
/* The transport struct itself lives with WA (03-kernel-impl.md §2     */
/* transport.c row); declaring it here would create two owners for it. */
/* ------------------------------------------------------------------ */

#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_DEFAULT

/* Byte-level driver seam.  The board glue that owns the actual UART
 * registers plugs in here; uart.c never touches a serial driver directly.
 * getc returns -1 when no byte is available (polling tier, 04 §1); putc
 * returns 0 or -errno. */
typedef struct a20_clx_uart_drv {
    void *ctx;
    int (*getc)(void *ctx);
    int (*putc)(void *ctx, uint8_t b);
} a20_clx_uart_drv_t;

typedef struct a20_clx_uart_link_cfg {
    uint8_t self_node_id[A20_CLX_NODE_ID_LEN]; /* head identity (set_self) */
    uint16_t assigned_addr;                    /* short addr announced in  */
                                               /* HELLO/HELLO_ACK          */
    a20_clx_uart_drv_t drv;
} a20_clx_uart_link_cfg_t;

/*
 * Register one point-to-point UART link (one leaf per UART, 04 §4).
 * Returns a link id >= 0, or -EINVAL on a bad cfg / -ENOSPC when the
 * static table is full.  Call before any poll/send; registration order is
 * the only ordering guarantee.
 */
int a20_clx_uart_link_register(const a20_clx_uart_link_cfg_t *cfg);
int a20_clx_uart_link_unregister(int link_id);

/*
 * 04 §1 send contract, UART flavour: next_hop is the 2-byte short address
 * (01-abi §cluster_route: "uart=短地址(2B)").  Routes the frame to the link
 * whose peer holds that short address; the frame must already carry its CRC
 * (csum_kind=1).  Returns 0, -EINVAL (bad args), -EMSGSIZE (over MTU: UART
 * forbids fragmentation, 02 §5), or -ENOENT (no UP link owns that address;
 * counted, and the frame is gone -- 04 §1 lets a transport drop).
 */
int a20_clx_uart_send_frame(const uint8_t *next_hop, uint32_t nh_len,
                            const uint8_t *frame, uint32_t len);

/* 04 §1 POLLING pump: drain RX bytes and run the HELLO/PING timers of every
 * registered link.  Call from one pump context; the send path may run from
 * the cluster TX thread concurrently. */
void a20_clx_uart_poll_all(void);
void a20_clx_uart_poll_link(int link_id);

/*
 * Uplink seams (04 §1 上行接口).  Default with nothing attached: frames are
 * counted (rx_drops) and dropped, and link transitions are ignored -- so
 * the transport is inert but harmless until WA's core lands.
 *
 * rx: a20_clx_rx_frame shape -- next_hop_src is the 2-byte short address of
 * the link the frame arrived on; frame is the decoded CL frame including
 * header and CRC (the transport has already screened magic/ver/csum_kind/
 * length/CRC; re-validating upstream is expected, not a bug).
 *
 * link_event: a20_clx_link_event shape -- up is 1 for UP, 0 for DOWN.
 */
typedef void (*a20_clx_uart_rx_fn)(void *ud, const uint8_t *next_hop_src,
                                   uint32_t nh_len, const uint8_t *frame,
                                   uint32_t len);
typedef void (*a20_clx_uart_link_event_fn)(void *ud, const uint8_t *next_hop,
                                           uint32_t nh_len, int up);
void a20_clx_uart_set_rx_handler(a20_clx_uart_rx_fn fn, void *ud);
void a20_clx_uart_set_link_event_handler(a20_clx_uart_link_event_fn fn,
                                         void *ud);

/*
 * Head-side machine states, 02 §6.  These are the internal values; they are
 * NOT the ABI numbers -- a20_clx_uart_get_status() maps them onto the
 * cluster_link_status output contract (A20_CLX_LINK_DOWN/SUSPECT/UP in
 * kernel/include/abi/native/types.h: HELLO_SENT and DOWN both report
 * LINK_DOWN, SUSPECT reports SUSPECT, UP reports UP).
 */
#define A20_CLX_UART_L_DOWN       0u
#define A20_CLX_UART_L_HELLO_SENT 1u /* reported as LINK_DOWN upstream */
#define A20_CLX_UART_L_UP         2u
#define A20_CLX_UART_L_SUSPECT    3u

typedef struct a20_clx_uart_link_status {
    int link_id;
    uint32_t state; /* A20_CLX_UART_L_* internal, see note above */
    uint16_t assigned_addr;
    uint16_t peer_addr; /* adopted from HELLO_ACK / HELLO */
    uint32_t rtt_us;    /* sliding mean, alpha = 1/8 (02 §6) */
    uint64_t last_hello_age_ms;
    a20_clx_link_counters_t counters;
} a20_clx_uart_link_status_t;

/* Returns 0 and fills out, or -ENOENT for an unknown link id. */
int a20_clx_uart_get_status(int link_id, a20_clx_uart_link_status_t *out);

/* Frames the send path could not route (no UP link owns the short addr). */
uint64_t a20_clx_uart_route_misses(void);

#endif /* CONFIG_CLUSTER_PROFILE >= DEFAULT */

#endif /* _CLUSTER_UART_H */
