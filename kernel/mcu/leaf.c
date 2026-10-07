/*
 * MCU leaf protocol face -- the answering end of the cluster UART link.
 *
 * WC1 track owns kernel/mcu/ (docs/cluster/impl-prompts.md 文件所有权表).
 * See kernel/mcu/leaf.h for the spec mapping and the handshake-direction
 * note; the wire codec and the frame screen are shared with the head node
 * through kernel/cluster/uart.h so both ends cannot drift apart, and the
 * UART bytes come from this tree's own driver (04 §4: "复用其 uart.c 驱动
 * 与 heap.c" -- nothing here allocates, so heap.c is reused only in the
 * sense that the leaf adds zero pressure to it).
 *
 * Frame types answered (04 §4 leaf constraint), everything else is dropped
 * and counted without being an error:
 *   HELLO      -> HELLO_ACK (passive; head assigns the short address)
 *   PING       -> PONG (payload echoed verbatim, 02 §2)
 *   CALL       -> CALL_REPLY, single in-flight transaction with a one-entry
 *                 (src_hash, txid) dedup cache so a re-delivered CALL
 *                 re-sends the cached reply instead of re-executing
 *                 (02 §4 at-most-once, MCU tier: A20_LIMIT_CLX_INFLIGHT_MCU)
 *   ERROR      -> sent for unknown dst_slot (02 §4 "槽位查找失败") and for
 *                 HELLO validation failures while the address is already
 *                 assigned; received ERROR frames have nothing to map to
 *                 (the leaf never calls) and are counted, not errors
 *   CLOSE      -> invalidates the cached in-flight reply for dst_slot
 *                 (02 §8), no reply is sent
 *   SEND/ACK/NACK, fragments, broadcast, unknown types -> drop + count
 *                 (04 §4: "丢弃计数，不视为错误").
 *
 * State machine (04 §4): exactly DISCONNECTED and UP.  UP is entered by
 * answering a valid HELLO; 50 s without a PING returns to DISCONNECTED and
 * voids the short address.  The idle watchdog and the 50 ms inter-byte
 * resync both run in a20_mcu_leaf_poll().
 *
 * Compile-time gating: the spec-mandated static buffers (512 + 320 B) are
 * compiled out of the stm32 QEMU bring-up probe image, which kernel/mcu/
 * main.c documents as sitting at its SRAM edge with no UART peer on the
 * other end; every real-board MCU build carries the full leaf.  Set
 * CONFIG_MCU_CLUSTER_LEAF=1 to force it on for a QEMU probe as well.
 */
#include "leaf.h"

#include "core/string.h"
#include "core/timer.h"
#include "drivers/char/uart.h"

/* The QEMU probe image is the one MCU configuration without a peer and
 * without SRAM slack (see kernel/mcu/main.c); everywhere else the leaf is
 * on.  This keeps ONE source file for all MCU builds (03-kernel-impl.md
 * §4: "禁止分裂文件"). */
#if defined(CONFIG_STM32_QEMU) && !defined(CONFIG_MCU_CLUSTER_LEAF)
#define A20_MCU_LEAF_ENABLED 0
#else
#define A20_MCU_LEAF_ENABLED 1
#endif

#if A20_MCU_LEAF_ENABLED

/* Application-level status for a CALL whose operator rejected the payload.
 * Value is A20_ERR_INVALID_ARGUMENT (kernel/include/ipc/ipc.h, verified
 * 2026-10); it travels inside the CALL_REPLY payload, which is application
 * space -- the ERROR frame's errno is cluster-only (02 §8). */
#define A20_MCU_LEAF_STATUS_INVALID 12u

/*
 * Static buffers, 04 §4: "单接收缓冲 512B 静态数组 + 单发送缓冲 320B".  The
 * RX buffer holds the DECODED frame (escape expansion never needs storage:
 * the decoder consumes the wire byte-by-byte, and escaping on TX streams
 * out through a20_clx_slip_emit()).  320 B = the 256 B frame limit plus the
 * spec's margin; frames are capped at MTU 256 by the screen anyway.
 */
static uint8_t leaf_rx_buf[512];
static uint8_t leaf_tx_buf[320];

static a20_clx_slip_rx_t leaf_rx;
static uint32_t leaf_state = A20_MCU_LEAF_DISCONNECTED;
static uint16_t leaf_short_addr = A20_CLX_UART_SHORT_UNASSIGNED;
/* Default demo identity, installed byte-exactly (16 chars, no NUL) by
 * a20_mcu_leaf_init(); a string-literal array initializer would truncate
 * a terminator and warn on newer GCC. */
static uint8_t leaf_self_id[A20_CLX_NODE_ID_LEN];
static uint8_t leaf_peer_id[A20_CLX_NODE_ID_LEN];
static uint64_t leaf_last_ping_ticks;
static uint64_t leaf_nonce;

/*
 * Single in-flight transaction (01-abi 限额: 待确认未应答 CALL, MCU = 1).
 * The leaf answers synchronously inside poll(), so the cache exists for
 * duplicate delivery: the same (src_hash, txid) re-sends the cached reply
 * frame instead of re-executing the operator (02 §4 at-most-once).  The
 * cached frames are only ever CALL_REPLY with the 12-byte operator reply
 * (46 B total) or an ERROR (42 B), so 64 B is enough with headroom; larger
 * builds are simply not cached (dedup then misses and the operator
 * re-executes, which for the built-in dot product is pure).
 */
static uint32_t leaf_inflight_src_hash;
static uint32_t leaf_inflight_txid;
static uint32_t leaf_inflight_slot;
static uint16_t leaf_inflight_len;
static uint8_t leaf_inflight_frame[64];

static a20_clx_link_counters_t leaf_c;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t leaf_next_nonce(uint64_t prev)
{
    /* Nonce is only ever compared for equality (02 §6 anti-loop); a
     * non-cryptographic never-repeating mix is sufficient. */
    return prev * 6364136223846793005ULL + 1442695040888963407ULL +
           timer_get_ticks();
}

static void leaf_disconnect(void)
{
    leaf_state = A20_MCU_LEAF_DISCONNECTED;
    leaf_short_addr = A20_CLX_UART_SHORT_UNASSIGNED; /* "短地址作废", 04 §4 */
    leaf_inflight_len = 0;
}

static int leaf_uart_sink(void *ctx, uint8_t b)
{
    (void)ctx;
    uart_putc((char)b);
    return 0;
}

/* Emit one frame as a SLIP packet (head END, escaped body, tail END).
 * The 0xFFFF TX gate lives here: an unassigned leaf may only put
 * HELLO-family frames on the wire (04 §4). */
static int leaf_output_frame(const uint8_t *frame, uint32_t len)
{
    uint32_t i;

    if (leaf_short_addr == A20_CLX_UART_SHORT_UNASSIGNED &&
        frame[A20_CLX_OFF_TYPE] != A20_CLX_TYPE_HELLO_ACK) {
        leaf_c.tx_drops++;
        return -1;
    }
    uart_putc((char)A20_CLX_SLIP_END);
    for (i = 0; i < len; i++) {
        if (a20_clx_slip_emit(frame[i], leaf_uart_sink, NULL) != 0) {
            leaf_c.tx_drops++;
            return -1;
        }
    }
    uart_putc((char)A20_CLX_SLIP_END);
    leaf_c.tx_frames++;
    return 0;
}

/* Build (header + payload + CRC) into the single 320 B TX buffer and send
 * it.  Returns 0 or -1 (counted by the caller or by leaf_output_frame). */
static int leaf_send(uint8_t type, uint32_t txid, uint32_t src_hash,
                     uint32_t dst_hash, uint32_t dst_slot,
                     const uint8_t *payload, uint16_t payload_len)
{
    int len = a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf), type, 0,
                                  0, txid, src_hash, dst_hash, dst_slot,
                                  payload, payload_len);

    if (len < 0) {
        leaf_c.tx_drops++;
        return -1;
    }
    return leaf_output_frame(leaf_tx_buf, (uint32_t)len);
}

static int leaf_screen(const uint8_t *frame, uint32_t len)
{
    int rc = a20_clx_screen_frame(frame, len);

    if (rc == A20_CLX_SCREEN_MALFORMED) {
        leaf_c.rx_malformed++;
        return -1;
    }
    if (rc == A20_CLX_SCREEN_DROP) { /* TTL exhausted, 02 §1 */
        leaf_c.rx_drops++;
        return -1;
    }
    return 0;
}

static int leaf_id_is_reserved(const uint8_t *id)
{
    int all_zero = 1, all_ff = 1;
    int i;

    for (i = 0; i < (int)A20_CLX_NODE_ID_LEN; i++) {
        if (id[i] != 0x00)
            all_zero = 0;
        if (id[i] != 0xFF)
            all_ff = 0;
    }
    return all_zero || all_ff;
}

static int leaf_is_self(const uint8_t *id)
{
    int i;

    for (i = 0; i < (int)A20_CLX_NODE_ID_LEN; i++)
        if (id[i] != leaf_self_id[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Frame handlers                                                      */
/* ------------------------------------------------------------------ */

/*
 * 02 §6 HELLO validation, leaf side.  Returns 0 when the leaf may adopt
 * the assignment and answer; -1 counts hello_rejects and stays
 * DISCONNECTED.  While the leaf holds an assigned address a failed
 * validation also sends ERROR (02 §6: "任何失败 -> ERROR 帧 + 状态到
 * DOWN"); while unassigned the 0xFFFF TX gate forbids the ERROR -- the
 * drop itself is the refusal an unassigned leaf can express.
 */
static int leaf_validate_hello(const uint8_t *frame)
{
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    const uint8_t *node_id = payload + A20_CLX_HELLO_OFF_NODE_ID;
    uint16_t payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    uint8_t proto_min = payload[A20_CLX_HELLO_OFF_PROTO_MIN];
    uint8_t tier = payload[A20_CLX_HELLO_OFF_TIER];
    uint8_t caps = payload[A20_CLX_HELLO_OFF_CAPS];
    int i, nonce_self = 1;

    if (frame[A20_CLX_OFF_VER] != (uint8_t)A20_CLX_WIRE_VER)
        return -1; /* 02 §1: foreign ver handled at HELLO stage */
    if (payload_len != (uint16_t)A20_CLX_HELLO_LEN)
        return -1;
    if (src_hash != a20_clx_fnv1a32(node_id))
        return -1; /* 01-abi: header hash must match the payload identity */
    if (leaf_id_is_reserved(node_id) || leaf_is_self(node_id))
        return -1;
    for (i = 0; i < 8; i++) {
        uint8_t nb = (uint8_t)(leaf_nonce >> (8 * i));
        if (payload[A20_CLX_HELLO_OFF_NONCE + i] != nb)
            nonce_self = 0;
    }
    if (nonce_self)
        return -1; /* 02 §6 self-loop */
    if (caps & ~(uint8_t)(A20_CLX_CAP_RELAY | A20_CLX_CAP_RELIABLE |
                          A20_CLX_CAP_LEAF))
        return -1;
    if (tier == A20_CLX_TIER_MCU && (caps & A20_CLX_CAP_RELAY))
        return -1; /* 02 §6: "叶子不得置 RELAY" -- also for a leaf peer */
    /* 02 §6: proto intersection with our v0 must be non-empty (unsigned,
     * so proto_max below 0 cannot exist). */
    if (proto_min > (uint8_t)A20_CLX_WIRE_VER)
        return -1;
    /* The head must assign a real short address (02 §6 payload field). */
    if (a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR) ==
        A20_CLX_UART_SHORT_UNASSIGNED)
        return -1;
    return 0;
}

static void leaf_on_hello(const uint8_t *frame)
{
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint16_t short_addr;
    uint8_t ack_payload[A20_CLX_HELLO_LEN];
    int rc;

    rc = leaf_validate_hello(frame);
    if (rc != 0) {
        leaf_c.hello_rejects++;
        if (leaf_state == A20_MCU_LEAF_UP) {
            /* Assigned: we may speak ERROR, and the failed handshake ends
             * the session (02 §6). */
            uint8_t err_payload[8];
            a20_clx_put_le32(err_payload, A20_CLX_WERR_CLUSTER_UNSUPPORTED);
            a20_clx_put_le32(err_payload + 4,
                             a20_clx_get_le32(frame + A20_CLX_OFF_TXID));
            (void)leaf_send(A20_CLX_TYPE_ERROR, 0,
                            a20_clx_fnv1a32(leaf_self_id),
                            a20_clx_fnv1a32(leaf_peer_id), 0, err_payload, 8);
            leaf_disconnect();
        }
        return;
    }

    short_addr = a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR);
    memcpy(leaf_peer_id, payload + A20_CLX_HELLO_OFF_NODE_ID,
           A20_CLX_NODE_ID_LEN);
    leaf_short_addr = short_addr; /* assignment adopted, 02 §6 */
    leaf_state = A20_MCU_LEAF_UP;
    leaf_last_ping_ticks = timer_get_ticks();
    leaf_inflight_len = 0; /* renegotiation resets the transaction state */

    /* Passive answer: HELLO_ACK echoes the adopted assignment (02 §6:
     * HELLO_ACK payload is "同 HELLO"). */
    leaf_nonce = leaf_next_nonce(leaf_nonce);
    a20_clx_build_hello_payload(leaf_self_id, (uint8_t)A20_CLX_WIRE_VER,
                                (uint8_t)A20_CLX_WIRE_VER,
                                (uint8_t)A20_CLX_TIER_MCU, A20_CLX_CAP_LEAF,
                                leaf_short_addr, leaf_nonce, ack_payload);
    if (leaf_send(A20_CLX_TYPE_HELLO_ACK, 0, a20_clx_fnv1a32(leaf_self_id),
                  a20_clx_fnv1a32(leaf_peer_id), 0, ack_payload,
                  (uint16_t)A20_CLX_HELLO_LEN) == 0)
        leaf_c.rx_frames++;
}

static void leaf_on_ping(const uint8_t *frame)
{
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint16_t payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    int len;

    if (leaf_state != A20_MCU_LEAF_UP) {
        leaf_c.rx_drops++; /* 04 §4: PING is an UP-phase heartbeat */
        return;
    }
    if (payload_len != 8 ||
        src_hash != a20_clx_fnv1a32(leaf_peer_id)) {
        leaf_c.rx_drops++;
        return;
    }
    leaf_last_ping_ticks = timer_get_ticks(); /* watchdog refresh, 04 §4 */
    len = leaf_send(A20_CLX_TYPE_PONG, 0, a20_clx_fnv1a32(leaf_self_id),
                    a20_clx_fnv1a32(leaf_peer_id), 0, payload, 8);
    if (len == 0)
        leaf_c.rx_frames++;
}

/* ------------------------------------------------------------------ */
/* The built-in demo operator: u32 vector dot product                  */
/* ------------------------------------------------------------------ */

/* CALL payload: u32 n; i32 a[n]; i32 b[n].  Returns 0 and the dot product,
 * or A20_MCU_LEAF_STATUS_INVALID when the payload is not exactly that. */
static uint32_t leaf_op_dot(const uint8_t *payload, uint16_t len,
                            int64_t *out)
{
    uint32_t n, i;
    uint64_t acc = 0;
    const uint8_t *a, *b;

    if (len < 4)
        return A20_MCU_LEAF_STATUS_INVALID;
    n = a20_clx_get_le32(payload);
    if ((uint32_t)len != 4u + 8u * n)
        return A20_MCU_LEAF_STATUS_INVALID;
    /* len <= 222 (MTU screen), so n <= 27 and the loops below are bound. */
    a = payload + 4;
    b = a + 4u * n;
    for (i = 0; i < n; i++) {
        int64_t av = (int64_t)(int32_t)a20_clx_get_le32(a + 4u * i);
        int64_t bv = (int64_t)(int32_t)a20_clx_get_le32(b + 4u * i);
        /* i32 * i32 fits i64; the unsigned accumulator wraps defined. */
        acc += (uint64_t)(av * bv);
    }
    *out = (int64_t)acc;
    return 0;
}

static void leaf_inflight_store(uint32_t src_hash, uint32_t txid,
                                uint32_t slot, const uint8_t *frame,
                                uint16_t len)
{
    if (len > sizeof(leaf_inflight_frame))
        return; /* not a shape this leaf emits; see the cache comment */
    leaf_inflight_src_hash = src_hash;
    leaf_inflight_txid = txid;
    leaf_inflight_slot = slot;
    leaf_inflight_len = len;
    memcpy(leaf_inflight_frame, frame, len);
}

static void leaf_on_call(const uint8_t *frame)
{
    uint32_t txid = a20_clx_get_le32(frame + A20_CLX_OFF_TXID);
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    uint32_t dst_hash = a20_clx_get_le32(frame + A20_CLX_OFF_DST_HASH);
    uint32_t slot = a20_clx_get_le32(frame + A20_CLX_OFF_DST_SLOT);
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint16_t payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    uint32_t self_hash = a20_clx_fnv1a32(leaf_self_id);
    uint32_t peer_hash = a20_clx_fnv1a32(leaf_peer_id);
    int len;

    if (leaf_state != A20_MCU_LEAF_UP) {
        leaf_c.rx_drops++; /* no service before the handshake */
        return;
    }
    if (src_hash != peer_hash || dst_hash != self_hash) {
        leaf_c.rx_drops++; /* not for us / not from our head */
        return;
    }
    if (leaf_inflight_len != 0 && leaf_inflight_src_hash == src_hash &&
        leaf_inflight_txid == txid) {
        /* 02 §4: re-delivered CALL -> re-send the cached reply. */
        (void)leaf_output_frame(leaf_inflight_frame, leaf_inflight_len);
        leaf_c.dedup_drops++;
        return;
    }
    leaf_c.rx_frames++;

    if (slot != A20_MCU_LEAF_OP_DOT_SLOT) {
        /* 02 §4: "槽位查找失败 -> ERROR(errno, T)" with the 01-abi errno. */
        uint8_t err_payload[8];
        a20_clx_put_le32(err_payload, A20_CLX_WERR_NOT_FOUND);
        a20_clx_put_le32(err_payload + 4, txid);
        len = a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf),
                                  A20_CLX_TYPE_ERROR, 0, 0, txid, self_hash,
                                  peer_hash, 0, err_payload, 8);
        if (len < 0) {
            leaf_c.tx_drops++;
            return;
        }
        leaf_inflight_store(src_hash, txid, slot, leaf_tx_buf,
                            (uint16_t)len);
        (void)leaf_output_frame(leaf_tx_buf, (uint32_t)len);
        return;
    }

    {
        uint8_t reply_payload[12];
        uint32_t status;
        int64_t dot = 0;

        status = leaf_op_dot(payload, payload_len, &dot);
        a20_clx_put_le32(reply_payload, status);
        a20_clx_put_le64(reply_payload + 4, (uint64_t)dot);
        len = a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf),
                                  A20_CLX_TYPE_CALL_REPLY, 0, 0, txid,
                                  self_hash, peer_hash, slot, reply_payload,
                                  (uint16_t)sizeof(reply_payload));
        if (len < 0) {
            leaf_c.tx_drops++;
            return;
        }
        leaf_inflight_store(src_hash, txid, slot, leaf_tx_buf,
                            (uint16_t)len);
        (void)leaf_output_frame(leaf_tx_buf, (uint32_t)len);
    }
}

static void leaf_on_close(const uint8_t *frame)
{
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    uint32_t dst_hash = a20_clx_get_le32(frame + A20_CLX_OFF_DST_HASH);
    uint32_t slot = a20_clx_get_le32(frame + A20_CLX_OFF_DST_SLOT);

    if (leaf_state != A20_MCU_LEAF_UP) {
        leaf_c.rx_drops++;
        return;
    }
    if (src_hash != a20_clx_fnv1a32(leaf_peer_id) ||
        dst_hash != a20_clx_fnv1a32(leaf_self_id)) {
        leaf_c.rx_drops++;
        return;
    }
    /* 02 §8: CLOSE names the closed endpoint via dst_slot; the leaf only
     * holds the one in-flight cache entry, so invalidating a match is the
     * whole teardown.  No reply. */
    if (leaf_inflight_len != 0 && leaf_inflight_slot == slot)
        leaf_inflight_len = 0;
    leaf_c.rx_frames++;
}

static void leaf_on_frame(const uint8_t *frame, uint32_t len)
{
    uint8_t type;
    uint16_t flags;

    if (leaf_screen(frame, len) != 0)
        return;
    type = frame[A20_CLX_OFF_TYPE];
    flags = a20_clx_get_le16(frame + A20_CLX_OFF_FLAGS);

    /* 02 §3/§5: no RELIABLE peer on this link, and the MCU tier forbids
     * fragments outright; broadcast is SEND-only and SERVER-tier. */
    if (flags & (A20_CLX_FLAG_RELIABLE | A20_CLX_FLAG_FRAGMENTED |
                 A20_CLX_FLAG_BROADCAST)) {
        leaf_c.rx_drops++;
        return;
    }

    switch (type) {
    case A20_CLX_TYPE_HELLO:
        leaf_on_hello(frame);
        return;
    case A20_CLX_TYPE_PING:
        leaf_on_ping(frame);
        return;
    case A20_CLX_TYPE_CALL:
        leaf_on_call(frame);
        return;
    case A20_CLX_TYPE_CLOSE:
        leaf_on_close(frame);
        return;
    case A20_CLX_TYPE_HELLO_ACK:
    case A20_CLX_TYPE_PONG:
    case A20_CLX_TYPE_CALL_REPLY:
    case A20_CLX_TYPE_ERROR:
        /* The leaf never dials, never pings and never calls, so these have
         * no local transaction to land on (02 §4: "迟到的 REPLY 按 txid 查
         * 无事务 -> 丢弃计数"). */
        leaf_c.rx_drops++;
        return;
    case A20_CLX_TYPE_SEND:
    case A20_CLX_TYPE_ACK:
    case A20_CLX_TYPE_NACK:
        /* 04 §4: "收到 SEND/ACK/NACK/分片帧：丢弃计数，不视为错误". */
        leaf_c.rx_drops++;
        return;
    default:
        leaf_c.rx_malformed++; /* 02 §1: unknown type */
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void a20_mcu_leaf_init(void)
{
    a20_clx_slip_rx_init(&leaf_rx, leaf_rx_buf, sizeof(leaf_rx_buf),
                         MS_TO_TICKS(A20_CLX_UART_INTERBYTE_MS));
    memcpy(leaf_self_id, "MCU-LEAF-DEMO-01", A20_CLX_NODE_ID_LEN);
    leaf_state = A20_MCU_LEAF_DISCONNECTED;
    leaf_short_addr = A20_CLX_UART_SHORT_UNASSIGNED;
    leaf_last_ping_ticks = 0;
    leaf_inflight_len = 0;
    memset(&leaf_c, 0, sizeof(leaf_c));
    /* Arm the anti-loop nonce with something per-boot so a zero nonce from
     * a peer is not misread as our own echo (02 §6). */
    leaf_nonce = leaf_next_nonce(timer_get_ticks() ^ 0xA20C1A55ULL);
}

void a20_mcu_leaf_set_identity(const uint8_t node_id[A20_CLX_NODE_ID_LEN])
{
    if (!node_id || leaf_id_is_reserved(node_id))
        return; /* LOCAL/BROADCAST are not wire identities (01-abi) */
    memcpy(leaf_self_id, node_id, A20_CLX_NODE_ID_LEN);
}

void a20_mcu_leaf_poll(void)
{
    uint64_t now;
    int c;

    now = timer_get_ticks();
    while ((c = uart_try_getc()) >= 0) {
        int r = a20_clx_slip_rx_byte(&leaf_rx, (uint8_t)c, now);
        if (r == 1)
            leaf_on_frame(leaf_rx.buf, leaf_rx.len);
        else if (r < 0)
            leaf_c.rx_drops++; /* half frame dropped, decoder re-synced */
    }
    /* 04 §4: inter-byte silence > 50 ms kills the half frame. */
    if (a20_clx_slip_rx_timeout(&leaf_rx, now))
        leaf_c.rx_drops++;

    /* 04 §4: "50s 无 PING 回 DISCONNECTED，短地址作废". */
    if (leaf_state == A20_MCU_LEAF_UP &&
        now - leaf_last_ping_ticks > MS_TO_TICKS(A20_CLX_UART_LEAF_IDLE_MS))
        leaf_disconnect();
}

void a20_mcu_leaf_get_status(a20_mcu_leaf_status_t *out)
{
    if (!out)
        return;
    out->state = leaf_state;
    out->short_addr = leaf_short_addr;
    out->counters = leaf_c;
}

void a20_mcu_leaf_on_frame(const uint8_t *frame, uint32_t len)
{
    leaf_on_frame(frame, len);
}

#else  /* !A20_MCU_LEAF_ENABLED */

/*
 * QEMU probe build: the leaf API exists, does nothing and costs no SRAM
 * (see the gating note at the top of this file).  One TU, no split file.
 */
void a20_mcu_leaf_init(void)
{
}

void a20_mcu_leaf_set_identity(const uint8_t node_id[A20_CLX_NODE_ID_LEN])
{
    (void)node_id;
}

void a20_mcu_leaf_poll(void)
{
}

void a20_mcu_leaf_get_status(a20_mcu_leaf_status_t *out)
{
    if (!out)
        return;
    out->state = A20_MCU_LEAF_DISCONNECTED;
    out->short_addr = A20_CLX_UART_SHORT_UNASSIGNED;
    memset(&out->counters, 0, sizeof(out->counters));
}

void a20_mcu_leaf_on_frame(const uint8_t *frame, uint32_t len)
{
    (void)frame;
    (void)len;
}

#endif /* A20_MCU_LEAF_ENABLED */
