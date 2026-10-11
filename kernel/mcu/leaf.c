/* Static-memory MCU leaf, interoperable with the hosted CL adapters.
 * v0 handles legacy single-frame calls; negotiated v1 shares session and
 * serial-window primitives with the host and replays one retained reply.
 * A full unacknowledged cache rejects admission instead of re-executing.
 * Board byte I/O is the only transport dependency. No heap allocation.
 * See docs/cluster/02-wire-protocol.md and 04-transports.md. */
#include "leaf.h"
#include "cluster/operator.h"
#include "cluster/transaction.h"
#include "uapi/a20/cluster_directory.h"

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
static uint64_t leaf_peer_nonce;
static uint8_t leaf_peer_version;
static a20_clx_window_t leaf_window;
static int leaf_reply_acknowledged;
static uint64_t leaf_reply_expires;
static int leaf_reply_reliable;

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
static uint8_t leaf_inflight_frame[A20_CLX_UART_MTU];

static uint64_t leaf_clock_ms(void *ctx) {
    (void)ctx;
    return timer_get_ticks() * 1000 / TICKS_PER_SEC;
}
static const a20_work_operator_t leaf_operators[] = {
    {A20_WORK_DOT, A20_WORK_REPLAY_SAFE, 140, 8, a20_work_dot, NULL},
    {A20_WORK_SUM64, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 136, 8,
     a20_work_sum64, NULL},
};
static const a20_work_agent_t leaf_default_agent = {
    .operators = leaf_operators,
    .operator_count = 2,
    .max_input = 140,
    .max_output = 168,
    .max_inflight = 1,
    .max_budget_ms = 30000,
    .clock_ms = leaf_clock_ms,
};
static const a20_work_agent_t *leaf_agent = &leaf_default_agent;

static a20_clx_link_counters_t leaf_c;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t leaf_next_nonce(uint64_t prev) {
    /* Nonce is only ever compared for equality (02 §6 anti-loop); a
     * non-cryptographic never-repeating mix is sufficient. */
    return prev * 6364136223846793005ULL + 1442695040888963407ULL +
           timer_get_ticks();
}

static void leaf_disconnect(void) {
    leaf_state = A20_MCU_LEAF_DISCONNECTED;
    leaf_short_addr = A20_CLX_UART_SHORT_UNASSIGNED; /* "短地址作废", 04 §4 */
    leaf_inflight_len = 0;
}

static int leaf_uart_sink(void *ctx, uint8_t b) {
    (void)ctx;
    uart_putc((char)b);
    return 0;
}

/* Emit one frame as a SLIP packet (head END, escaped body, tail END).
 * The 0xFFFF TX gate lives here: an unassigned leaf may only put
 * HELLO-family frames on the wire (04 §4). */
static int leaf_output_frame(const uint8_t *frame, uint32_t len) {
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
                     const uint8_t *payload, uint16_t payload_len) {
    int len =
        a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf), type, 0, 0, txid,
                            src_hash, dst_hash, dst_slot, payload, payload_len);

    if (len < 0) {
        leaf_c.tx_drops++;
        return -1;
    }
    return leaf_output_frame(leaf_tx_buf, (uint32_t)len);
}

static int leaf_screen(const uint8_t *frame, uint32_t len) {
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

static int leaf_id_is_reserved(const uint8_t *id) {
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
static int leaf_validate_hello(const uint8_t *frame) {
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    if (a20_clx_validate_hello(
            payload, a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN),
            frame[A20_CLX_OFF_VER],
            a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH), leaf_self_id,
            leaf_nonce, 2) < 0)
        return -1;
    return a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR) ==
                   A20_CLX_UART_SHORT_UNASSIGNED
               ? -1
               : 0;
}

static void leaf_on_hello(const uint8_t *frame) {
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
    leaf_peer_version =
        payload[A20_CLX_HELLO_OFF_PROTO_MAX] >= A20_CLX_SESSION_VER
            ? A20_CLX_SESSION_VER
            : A20_CLX_WIRE_VER;
    leaf_last_ping_ticks = timer_get_ticks();
    uint64_t peer_nonce = a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE);
    if (leaf_peer_nonce != peer_nonce) {
        leaf_inflight_len = 0;
        memset(&leaf_window, 0, sizeof(leaf_window));
        leaf_peer_nonce = peer_nonce;
    }

    /* Passive answer: HELLO_ACK echoes the adopted assignment (02 §6:
     * HELLO_ACK payload is "同 HELLO"). */
    a20_clx_build_hello_payload(
        leaf_self_id, (uint8_t)A20_CLX_WIRE_VER, (uint8_t)A20_CLX_SESSION_VER,
        (uint8_t)A20_CLX_TIER_MCU, A20_CLX_CAP_LEAF | A20_CLX_CAP_RELIABLE,
        leaf_short_addr, leaf_nonce, ack_payload);
    if (leaf_send(A20_CLX_TYPE_HELLO_ACK, 0, a20_clx_fnv1a32(leaf_self_id),
                  a20_clx_fnv1a32(leaf_peer_id), 0, ack_payload,
                  (uint16_t)A20_CLX_HELLO_LEN) == 0)
        leaf_c.rx_frames++;
}

static void leaf_on_ping(const uint8_t *frame) {
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint16_t payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    int len;

    if (leaf_state != A20_MCU_LEAF_UP) {
        leaf_c.rx_drops++; /* 04 §4: PING is an UP-phase heartbeat */
        return;
    }
    if (payload_len != 8 || src_hash != a20_clx_fnv1a32(leaf_peer_id)) {
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

static void leaf_inflight_store(uint32_t src_hash, uint32_t txid, uint32_t slot,
                                const uint8_t *frame, uint16_t len) {
    if (len > sizeof(leaf_inflight_frame))
        return; /* not a shape this leaf emits; see the cache comment */
    leaf_inflight_src_hash = src_hash;
    leaf_inflight_txid = txid;
    leaf_inflight_slot = slot;
    leaf_inflight_len = len;
    memcpy(leaf_inflight_frame, frame, len);
}

static void leaf_set_reliable_seq(uint8_t *frame, uint32_t len, uint32_t txid) {
    frame[A20_CLX_OFF_VER] = A20_CLX_SESSION_VER;
    a20_clx_put_le32(frame + A20_CLX_OFF_SEQ, txid);
    a20_clx_put_le16(frame + len - 2, a20_clx_crc16_ccitt(frame, len - 2));
}

static void leaf_admission_ack(uint32_t txid, uint32_t destination) {
    uint8_t payload[4];
    a20_clx_put_le32(payload, txid);
    int n =
        a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf), A20_CLX_TYPE_ACK,
                            0, 0, txid, a20_clx_fnv1a32(leaf_self_id),
                            destination, 0, payload, sizeof(payload));
    if (n > 0) {
        leaf_tx_buf[A20_CLX_OFF_VER] = A20_CLX_SESSION_VER;
        a20_clx_put_le16(leaf_tx_buf + n - 2,
                         a20_clx_crc16_ccitt(leaf_tx_buf, n - 2));
        leaf_output_frame(leaf_tx_buf, n);
    }
}

static int leaf_directory(const uint8_t *input, uint32_t len, uint8_t *output) {
    a20_directory_record_t r = {.profile = A20_CLX_PROFILE_MCU,
                                .caps = A20_CLX_CAP_LEAF | A20_CLX_CAP_RELIABLE,
                                .boot_epoch = leaf_nonce,
                                .generation = 1,
                                .status = A20_DIRECTORY_INVALID};
    memcpy(r.node_id, leaf_self_id, 16);
    uint32_t index;
    uint64_t generation;
    if (!a20_directory_decode_request(input, len, &index, &generation)) {
        if (generation && generation != 1) {
            r.status = A20_DIRECTORY_CHANGED;
        } else if (index >= 2) {
            r.status = A20_DIRECTORY_END;
        } else {
            r.status = A20_DIRECTORY_FOUND;
            r.next_index = index + 1;
            r.slot = index ? A20_MCU_LEAF_WORK_SLOT : A20_MCU_LEAF_OP_DOT_SLOT;
            r.flags = index ? A20_DIRECTORY_WORK : 0;
            r.name_len = index ? 4 : 3;
            memcpy(r.name, index ? "work" : "dot", r.name_len);
        }
    }
    a20_directory_encode(output, &r);
    return A20_DIRECTORY_REPLY;
}

static void leaf_on_call(const uint8_t *frame) {
    uint32_t txid = a20_clx_get_le32(frame + A20_CLX_OFF_TXID);
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    uint32_t dst_hash = a20_clx_get_le32(frame + A20_CLX_OFF_DST_HASH);
    uint32_t slot = a20_clx_get_le32(frame + A20_CLX_OFF_DST_SLOT);
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint16_t payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    uint32_t self_hash = a20_clx_fnv1a32(leaf_self_id);
    int reliable =
        !!(a20_clx_get_le16(frame + A20_CLX_OFF_FLAGS) & A20_CLX_FLAG_RELIABLE);
    uint32_t error = 0;
    if (leaf_state != A20_MCU_LEAF_UP || dst_hash != self_hash ||
        src_hash != a20_clx_fnv1a32(leaf_peer_id)) {
        leaf_c.rx_drops++;
        return;
    }
    if (reliable) {
        if (frame[A20_CLX_OFF_VER] != A20_CLX_SESSION_VER ||
            leaf_peer_version < A20_CLX_SESSION_VER ||
            !a20_clx_session_matches(payload, payload_len, leaf_peer_nonce,
                                     leaf_nonce) ||
            a20_clx_get_le32(frame + A20_CLX_OFF_SEQ) != txid) {
            leaf_c.rx_drops++;
            return;
        }
        if (!slot) {
            if (payload_len == A20_CLX_SESSION_BYTES && leaf_reply_reliable &&
                txid == leaf_inflight_txid)
                leaf_reply_acknowledged = 1;
            return;
        }
        payload += A20_CLX_SESSION_BYTES;
        payload_len -= A20_CLX_SESSION_BYTES;
    }
    if (leaf_inflight_len &&
        (!reliable || timer_get_ticks() < leaf_reply_expires) &&
        leaf_inflight_src_hash == src_hash && leaf_inflight_txid == txid &&
        leaf_reply_reliable == reliable) {
        if (reliable)
            leaf_admission_ack(txid, src_hash);
        leaf_output_frame(leaf_inflight_frame, leaf_inflight_len);
        leaf_c.dedup_drops++;
        return;
    }
    int protected_reply = leaf_inflight_len && leaf_reply_reliable &&
                          !leaf_reply_acknowledged &&
                          timer_get_ticks() < leaf_reply_expires;
    if (!reliable && protected_reply)
        error = 13;
    if (reliable) {
        if (a20_clx_window_test(&leaf_window, txid) != A20_CLX_TX_NEW)
            error = 20; /* A20_ERR_CANCELED: old transaction, never execute. */
        else if (protected_reply)
            error = 13; /* A20_ERR_NO_SPACE */
    }
    uint8_t reply[A20_CLX_UART_MTU - A20_CLX_HDR_LEN - A20_CLX_CRC_LEN];
    uint32_t prefix = reliable ? A20_CLX_REPLY_PREFIX : 0;
    uint32_t reply_len = 0;
    if (!error && slot != A20_MCU_LEAF_OP_DOT_SLOT &&
        slot != A20_MCU_LEAF_WORK_SLOT && slot != A20_DIRECTORY_SLOT)
        error = A20_CLX_WERR_NOT_FOUND;
    if (!error) {
        if (slot == A20_DIRECTORY_SLOT) {
            reply_len = leaf_directory(payload, payload_len, reply + prefix);
        } else if (slot == A20_MCU_LEAF_WORK_SLOT) {
            int result =
                a20_work_dispatch(leaf_agent, payload, payload_len,
                                  reply + prefix, sizeof(reply) - prefix);
            if (result < 0)
                error = 13;
            else
                reply_len = (uint32_t)result;
        } else {
            a20_clx_operator_dot(payload, payload_len, reply + prefix,
                                 sizeof(reply) - prefix);
            reply_len = A20_CLX_DOT_REPLY_BYTES;
        }
    }
    int n;
    if (reliable) {
        a20_clx_session_encode(reply, leaf_nonce, leaf_peer_nonce);
        a20_clx_put_le32(reply + A20_CLX_SESSION_BYTES, error);
        n = a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf),
                                A20_CLX_TYPE_CALL_REPLY, A20_CLX_FLAG_RELIABLE,
                                0, txid, self_hash, src_hash, slot, reply,
                                prefix + reply_len);
        if (n > 0)
            leaf_set_reliable_seq(leaf_tx_buf, n, txid);
    } else if (error) {
        a20_clx_put_le32(reply, error);
        a20_clx_put_le32(reply + 4, txid);
        n = a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf),
                                A20_CLX_TYPE_ERROR, 0, 0, txid, self_hash,
                                src_hash, 0, reply, 8);
    } else {
        n = a20_clx_build_frame(leaf_tx_buf, sizeof(leaf_tx_buf),
                                A20_CLX_TYPE_CALL_REPLY, 0, 0, txid, self_hash,
                                src_hash, slot, reply, reply_len);
    }
    if (n <= 0) {
        leaf_c.tx_drops++;
        return;
    }
    if ((!reliable && !protected_reply) ||
        (reliable && (!error || error == A20_CLX_WERR_NOT_FOUND))) {
        leaf_inflight_store(src_hash, txid, slot, leaf_tx_buf, n);
        leaf_reply_reliable = reliable;
        leaf_reply_acknowledged = 0;
        leaf_reply_expires = timer_get_ticks() + MS_TO_TICKS(A20_CLX_RETAIN_MS);
        if (reliable)
            a20_clx_window_commit(&leaf_window, txid);
    }
    if (reliable && (!error || error == A20_CLX_WERR_NOT_FOUND))
        leaf_admission_ack(txid, src_hash);
    /* leaf_admission_ack uses leaf_tx_buf; the cache pins the admitted
     * reply. Rejected requests were not ACKed and still occupy tx_buf. */
    if (reliable && (!error || error == A20_CLX_WERR_NOT_FOUND))
        leaf_output_frame(leaf_inflight_frame, leaf_inflight_len);
    else
        leaf_output_frame(leaf_tx_buf, n);
    leaf_c.rx_frames++;
}

static void leaf_on_close(const uint8_t *frame) {
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
    if (!leaf_reply_reliable && leaf_inflight_len != 0 &&
        leaf_inflight_slot == slot)
        leaf_inflight_len = 0;
    leaf_c.rx_frames++;
}

static void leaf_on_frame(const uint8_t *frame, uint32_t len) {
    uint8_t type;
    uint16_t flags;

    if (leaf_screen(frame, len) != 0)
        return;
    type = frame[A20_CLX_OFF_TYPE];
    if (type != A20_CLX_TYPE_HELLO &&
        frame[A20_CLX_OFF_VER] > leaf_peer_version) {
        leaf_c.rx_drops++;
        return;
    }
    flags = a20_clx_get_le16(frame + A20_CLX_OFF_FLAGS);

    /* 02 §3/§5: no RELIABLE peer on this link, and the MCU tier forbids
     * fragments outright; broadcast is SEND-only and SERVER-tier. */
    if (flags & (A20_CLX_FLAG_FRAGMENTED | A20_CLX_FLAG_BROADCAST)) {
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

void a20_mcu_leaf_init(void) {
    a20_clx_slip_rx_init(&leaf_rx, leaf_rx_buf, sizeof(leaf_rx_buf),
                         MS_TO_TICKS(A20_CLX_UART_INTERBYTE_MS));
    memcpy(leaf_self_id, "MCU-LEAF-DEMO-01", A20_CLX_NODE_ID_LEN);
    leaf_state = A20_MCU_LEAF_DISCONNECTED;
    leaf_short_addr = A20_CLX_UART_SHORT_UNASSIGNED;
    leaf_last_ping_ticks = 0;
    leaf_inflight_len = 0;
    leaf_peer_nonce = 0;
    leaf_reply_reliable = 0;
    memset(&leaf_window, 0, sizeof(leaf_window));
    memset(&leaf_c, 0, sizeof(leaf_c));
    /* Arm the anti-loop nonce with something per-boot so a zero nonce from
     * a peer is not misread as our own echo (02 §6). */
    leaf_nonce = leaf_next_nonce(timer_get_ticks() ^ 0xA20C1A55ULL);
    leaf_agent = &leaf_default_agent;
}

int a20_mcu_leaf_set_agent(const a20_work_agent_t *agent) {
    if (leaf_state != A20_MCU_LEAF_DISCONNECTED ||
        !a20_work_agent_valid(agent) || agent->process ||
        agent->max_input > 140 || agent->max_output > 168 ||
        agent->max_inflight != 1 || agent->operator_count > 8 ||
        agent->max_budget_ms > 30000)
        return -1;
    leaf_agent = agent;
    return 0;
}

int a20_mcu_leaf_configure(const uint8_t node_id[16], uint64_t boot_epoch) {
    if (leaf_state != A20_MCU_LEAF_DISCONNECTED || !node_id ||
        leaf_id_is_reserved(node_id) || !boot_epoch)
        return -1;
    memcpy(leaf_self_id, node_id, 16);
    leaf_nonce = boot_epoch;
    leaf_peer_nonce = 0;
    leaf_inflight_len = 0;
    memset(&leaf_window, 0, sizeof(leaf_window));
    return 0;
}

void a20_mcu_leaf_set_identity(const uint8_t node_id[A20_CLX_NODE_ID_LEN]) {
    if (!node_id || leaf_id_is_reserved(node_id))
        return; /* LOCAL/BROADCAST are not wire identities (01-abi) */
    memcpy(leaf_self_id, node_id, A20_CLX_NODE_ID_LEN);
}

void a20_mcu_leaf_poll(void) {
    uint64_t now;
    int c;

    now = timer_get_ticks();
    uint32_t rx_budget = 64;
    while (rx_budget-- && (c = uart_try_getc()) >= 0) {
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

void a20_mcu_leaf_get_status(a20_mcu_leaf_status_t *out) {
    if (!out)
        return;
    out->state = leaf_state;
    out->short_addr = leaf_short_addr;
    out->counters = leaf_c;
}

void a20_mcu_leaf_on_frame(const uint8_t *frame, uint32_t len) {
    leaf_on_frame(frame, len);
}

#else /* !A20_MCU_LEAF_ENABLED */

int a20_mcu_leaf_set_agent(const a20_work_agent_t *agent) {
    (void)agent;
    return -1;
}
int a20_mcu_leaf_configure(const uint8_t node_id[16], uint64_t boot_epoch) {
    (void)node_id;
    (void)boot_epoch;
    return -1;
}

/*
 * QEMU probe build: the leaf API exists, does nothing and costs no SRAM
 * (see the gating note at the top of this file).  One TU, no split file.
 */
void a20_mcu_leaf_init(void) {
}

void a20_mcu_leaf_set_identity(const uint8_t node_id[A20_CLX_NODE_ID_LEN]) {
    (void)node_id;
}

void a20_mcu_leaf_poll(void) {
}

void a20_mcu_leaf_get_status(a20_mcu_leaf_status_t *out) {
    if (!out)
        return;
    out->state = A20_MCU_LEAF_DISCONNECTED;
    out->short_addr = A20_CLX_UART_SHORT_UNASSIGNED;
    memset(&out->counters, 0, sizeof(out->counters));
}

void a20_mcu_leaf_on_frame(const uint8_t *frame, uint32_t len) {
    (void)frame;
    (void)len;
}

#endif /* A20_MCU_LEAF_ENABLED */
