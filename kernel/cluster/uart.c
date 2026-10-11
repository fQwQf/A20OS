/* UART head adapter: bounded SLIP receive, handshake/heartbeat and a
 * single byte-stream writer. The poll owner mutates the state machine and
 * publishes snapshots under g_uart_lock; concurrent senders only enqueue.
 * Driver I/O and upcalls run outside locks. Register/unregister refuse an
 * active per-link pump. See docs/cluster/04-transports.md. */
#include "cluster/uart.h"
#include <stdint.h>

#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/timer.h"

/* The head-node machine below is DEFAULT/SERVER-tier work (03 §4: the MCU
 * tier gets the leaf in kernel/mcu/leaf.c instead).  Kept as a compile-time
 * guard rather than a Makefile exclusion so that one source file serves all
 * three tiers, per 03 §4 "禁止分裂文件". */
#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_DEFAULT

typedef struct a20_clx_uart_link {
    int id;
    int used;
    uint8_t self_node_id[A20_CLX_NODE_ID_LEN];
    uint16_t assigned_addr; /* what we announce in HELLO/HELLO_ACK */
    uint16_t peer_addr;     /* adopted from HELLO_ACK / peer HELLO     */
    a20_clx_uart_drv_t drv;

    a20_clx_slip_rx_t rx;
    uint8_t rx_buf[512]; /* same static sizing the leaf mandates, 04 §4 */

    uint8_t state; /* A20_CLX_UART_L_* */
    uint8_t hello_retries;
    uint32_t hello_cookie;
    uint8_t pings_missed;
    uint8_t awaiting_pong;
    uint64_t hello_sent_ticks;
    uint64_t last_ping_ticks;
    uint64_t ping_ts_us;    /* timestamp we put into the open PING  */
    uint64_t hello_done_ms; /* tick of the last completed handshake */
    uint64_t nonce;         /* last nonce we put on the wire        */
    uint8_t peer_node_id[A20_CLX_NODE_ID_LEN];
    uint32_t rtt_us;
    uint64_t peer_nonce;
    uint32_t peer_caps, self_caps;
    uint8_t peer_version;
    a20_clx_uart_link_status_t published;
    uint8_t published_node[16];
    uint64_t published_local, published_remote;
    uint32_t published_caps;
    int pumping;
    a20_clx_uart_rx_fn rx_handler;
    a20_clx_uart_link_event_fn event_handler;
    void *rx_userdata, *event_userdata;
    a20_clx_link_counters_t c;
    uint8_t tx_queue[4][A20_CLX_UART_MTU];
    uint16_t tx_len[4];
    uint32_t tx_head, tx_count;
} a20_clx_uart_link_t;

static a20_clx_uart_link_t g_uart_links[A20_CLX_UART_MAX_LINKS];
static spinlock_t g_uart_lock = SPINLOCK_INIT;
static a20_clx_uart_rx_fn g_uart_rx_fn;
static void *g_uart_rx_ud;
static a20_clx_uart_link_event_fn g_uart_event_fn;
static void *g_uart_event_ud;
static uint64_t g_uart_route_misses;
static uint32_t g_uart_hello_counter;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t uart_now_us(void) {
    /* timer.h ticks -> microseconds; 1e6-scale product overflows u64 only
     * after centuries at any plausible tick rate. */
    return timer_get_ticks() * 1000000ULL / TICKS_PER_SEC;
}

static uint64_t uart_next_nonce(uint64_t prev) {
    /* 02 §6 needs the nonce only for the self-loop equality test, so a
     * non-cryptographic mix that never repeats while the link is up is
     * sufficient; random_u64() would order this file against random_init()
     * for no gain. */
    return prev * 6364136223846793005ULL + 1442695040888963407ULL +
           timer_get_ticks();
}

/* Byte sink for a20_clx_slip_emit(): forwards into the link's driver. */
static int uart_putc_sink(void *ctx, uint8_t b) {
    a20_clx_uart_link_t *l = (a20_clx_uart_link_t *)ctx;

    return l->drv.putc(l->drv.ctx, b);
}

/* SLIP-stream one frame out through the driver, 04 §4 variant: head END,
 * escaped body, tail END.  The caller has already decided the link may
 * transmit; the putc busy-wait runs with IRQs on, outside g_uart_lock. */
static int uart_link_output(a20_clx_uart_link_t *l, const uint8_t *frame,
                            uint32_t len) {
    uint32_t i;

    /* 04 §4: head END first -- it is the receiver's re-sync point. */
    if (l->drv.putc(l->drv.ctx, A20_CLX_SLIP_END) != 0)
        goto fail;
    for (i = 0; i < len; i++) {
        if (a20_clx_slip_emit(frame[i], uart_putc_sink, l) != 0)
            goto fail;
    }
    if (l->drv.putc(l->drv.ctx, A20_CLX_SLIP_END) != 0)
        goto fail;
    l->c.tx_frames++;
    return 0;

fail:
    /* Truncated frame on the wire: the peer's 50 ms inter-byte timer is the
     * recovery path (04 §4). */
    l->c.tx_drops++;
    return -EINVAL;
}

/* 02 §6 HELLO payload, 32 bytes fixed, via the shared builder in uart.h. */
static void uart_fill_hello_payload(a20_clx_uart_link_t *l,
                                    uint8_t *p /* >= 32 bytes */,
                                    uint16_t short_addr, uint64_t nonce) {
    a20_clx_build_hello_payload(
        l->self_node_id, (uint8_t)A20_CLX_WIRE_VER,
        (uint8_t)A20_CLX_SESSION_VER, (uint8_t)CONFIG_CLUSTER_PROFILE,
        l->self_caps | A20_CLX_CAP_RELIABLE, short_addr, nonce, p);
}

static int uart_send_hello(a20_clx_uart_link_t *l, uint64_t now_ticks) {
    uint8_t payload[A20_CLX_HELLO_LEN];
    uint8_t frame[A20_CLX_HDR_LEN + A20_CLX_HELLO_LEN + A20_CLX_CRC_LEN];
    int len;

    if (!l->hello_cookie) {
        uint64_t irq = spin_lock_irqsave(&g_uart_lock);
        if (!g_uart_hello_counter)
            g_uart_hello_counter = (uint32_t)(l->nonce ^ (l->nonce >> 32)) | 1u;
        if (g_uart_hello_counter == UINT32_MAX) {
            spin_unlock_irqrestore(&g_uart_lock, irq);
            return -ENOSPC;
        }
        l->hello_cookie = ++g_uart_hello_counter;
        spin_unlock_irqrestore(&g_uart_lock, irq);
        l->hello_retries = 0;
    }
    uart_fill_hello_payload(l, payload, l->assigned_addr, l->nonce);
    /* Dial HELLO: the leaf's identity is not known yet, so dst_hash stays 0
     * ("no destination resolved"); the leaf does not filter HELLO by
     * dst_hash on a point-to-point wire. */
    len = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_HELLO, 0, 0,
                              l->hello_cookie, a20_clx_fnv1a32(l->self_node_id),
                              0, 0, payload, (uint16_t)A20_CLX_HELLO_LEN);
    if (len < 0)
        return -EINVAL;
    if (l->state != A20_CLX_UART_L_UP && l->state != A20_CLX_UART_L_SUSPECT)
        l->state = A20_CLX_UART_L_HELLO_SENT;
    l->hello_sent_ticks = now_ticks;
    return uart_link_output(l, frame, (uint32_t)len);
}

/*
 * 02 §6 HELLO validation, shared by HELLO and HELLO_ACK.  frame_ver is the
 * ver byte of the frame carrying this payload (02 §1: "ver 高于本地时在
 * HELLO 阶段处理" -- a too-new HELLO fails here as a negotiation failure
 * instead of being silently dropped).  Returns 0 when the peer may come UP,
 * -EINVAL on a negotiation failure (02 §6: "任何失败 -> ERROR 帧 + 状态到
 * DOWN"), -ENOENT for the self-loop case, which per 02 §6 is rejected
 * without an ERROR (answering yourself is pointless).
 */
static int uart_validate_hello(a20_clx_uart_link_t *l, const uint8_t *frame,
                               uint8_t *peer_caps_out) {
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    int r = a20_clx_validate_hello(
        payload, a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN),
        frame[A20_CLX_OFF_VER], a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH),
        l->self_node_id, l->nonce, 2);
    if (r < 0)
        return r == -2 ? -ENOENT : -EINVAL;
    *peer_caps_out = payload[A20_CLX_HELLO_OFF_CAPS];
    if (payload[A20_CLX_HELLO_OFF_PROTO_MAX] < A20_CLX_SESSION_VER)
        *peer_caps_out &= ~A20_CLX_CAP_RELIABLE;
    return 0;
}

/* 02 §6: "任何失败 -> ERROR 帧 + 状态到 DOWN". */
static void uart_send_hello_error(a20_clx_uart_link_t *l, uint32_t orig_txid) {
    uint8_t payload[8];
    uint8_t frame[A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN];
    int len;

    a20_clx_put_le32(payload, A20_CLX_WERR_CLUSTER_UNSUPPORTED);
    a20_clx_put_le32(payload + 4, orig_txid);
    len = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_ERROR, 0, 0, 0,
                              a20_clx_fnv1a32(l->self_node_id),
                              a20_clx_fnv1a32(l->peer_node_id), 0, payload, 8);
    if (len >= 0)
        (void)uart_link_output(l, frame, (uint32_t)len);
}

static void uart_link_down(a20_clx_uart_link_t *l, uint64_t now_ticks) {
    if (l->state != A20_CLX_UART_L_DOWN) {
        l->state = A20_CLX_UART_L_DOWN;
        l->awaiting_pong = 0;
        l->pings_missed = 0;
        l->hello_retries = 0;
        l->hello_cookie = 0;
        uint64_t irq = spin_lock_irqsave(&g_uart_lock);
        l->tx_count = 0;
        l->tx_head = 0;
        spin_unlock_irqrestore(&g_uart_lock, irq);
        l->hello_sent_ticks = now_ticks; /* also the redial cooldown stamp */
        uint8_t nh[2];
        a20_clx_put_le16(nh, l->peer_addr);
        if (l->event_handler)
            l->event_handler(l->event_userdata, nh, 2, 0);
        kinfo("cluster-uart: link %d DOWN\n", l->id);
    }
}

static void uart_link_up(a20_clx_uart_link_t *l, uint16_t peer_addr,
                         const uint8_t *peer_node_id, uint64_t now_ticks) {
    int was_down =
        (l->state != A20_CLX_UART_L_UP && l->state != A20_CLX_UART_L_SUSPECT);

    if (l->peer_addr != peer_addr)
        kinfo("cluster-uart: link %d peer short addr 0x%04x\n", l->id,
              peer_addr);
    l->peer_addr = peer_addr;
    memcpy(l->peer_node_id, peer_node_id, A20_CLX_NODE_ID_LEN);
    l->state = A20_CLX_UART_L_UP;
    l->pings_missed = 0;
    l->awaiting_pong = 0;
    l->hello_done_ms = now_ticks;
    /* First PING one period after the handshake, 04 §5. */
    l->last_ping_ticks = now_ticks;
    uint8_t nh[2];
    a20_clx_put_le16(nh, l->peer_addr);
    if (was_down && l->event_handler)
        l->event_handler(l->event_userdata, nh, 2, 1);
    kinfo("cluster-uart: link %d UP addr=0x%04x\n", l->id, peer_addr);
}

/* ------------------------------------------------------------------ */
/* Receive path                                                        */
/* ------------------------------------------------------------------ */

/*
 * Transport-level screen (shared rules in uart.h) mapped onto this link's
 * 02 §10 counters.  Frame version and unknown types are dispatch decisions
 * (uart_on_frame); returns 0 when the frame may proceed, -1 when it was
 * counted and dropped.
 */
static int uart_screen_frame(a20_clx_uart_link_t *l, const uint8_t *frame,
                             uint32_t len) {
    int rc = a20_clx_screen_frame(frame, len);

    if (rc == A20_CLX_SCREEN_MALFORMED) {
        l->c.rx_malformed++;
        return -1;
    }
    if (rc == A20_CLX_SCREEN_DROP) { /* TTL exhausted, 02 §1 */
        l->c.rx_drops++;
        return -1;
    }
    return 0;
}

static void uart_on_ack_frame(a20_clx_uart_link_t *l, const uint8_t *frame) {
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint32_t txid = a20_clx_get_le32(frame + A20_CLX_OFF_TXID);
    uint8_t peer_caps = 0;
    uint16_t short_addr;
    int rc;

    int legacy = payload[A20_CLX_HELLO_OFF_PROTO_MAX] < A20_CLX_SESSION_VER;
    if (!l->hello_cookie ||
        (txid != l->hello_cookie &&
         !(legacy && !txid && l->state == A20_CLX_UART_L_HELLO_SENT)) ||
        (legacy && l->peer_version == A20_CLX_SESSION_VER &&
         (l->state == A20_CLX_UART_L_UP ||
          l->state == A20_CLX_UART_L_SUSPECT))) {
        l->c.rx_drops++;
        return;
    }
    rc = uart_validate_hello(l, frame, &peer_caps);
    if (rc == -ENOENT) {
        /* Self-loop: reject without an ERROR (02 §6 via the nonce rule). */
        l->c.hello_rejects++;
        return;
    }
    if (rc != 0) {
        l->c.hello_rejects++;
        uart_send_hello_error(l, txid);
        uart_link_down(l, timer_get_ticks());
        return;
    }
    short_addr = a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR);
    /* The leaf echoes the assignment; an unassigned echo means the leaf
     * never adopted it -- not a link, 04 §4. */
    if (short_addr == A20_CLX_UART_SHORT_UNASSIGNED) {
        l->c.hello_rejects++;
        uart_send_hello_error(l, txid);
        uart_link_down(l, timer_get_ticks());
        return;
    }
    uint64_t new_nonce = a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE);
    if ((l->state == A20_CLX_UART_L_UP || l->state == A20_CLX_UART_L_SUSPECT) &&
        (l->peer_nonce != new_nonce ||
         memcmp(l->peer_node_id, payload + A20_CLX_HELLO_OFF_NODE_ID, 16)))
        uart_link_down(l, timer_get_ticks());
    l->hello_cookie = 0;
    l->peer_nonce = a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE);
    l->peer_caps = peer_caps;
    l->peer_version =
        payload[A20_CLX_HELLO_OFF_PROTO_MAX] >= A20_CLX_SESSION_VER
            ? A20_CLX_SESSION_VER
            : A20_CLX_WIRE_VER;
    /* Adopt what the peer echoed, not what we announced: 04 §4 头节点侧
     * requires the head to handle a leaf that came back with a new short
     * address after a renegotiation. */
    uart_link_up(l, short_addr, payload + A20_CLX_HELLO_OFF_NODE_ID,
                 timer_get_ticks());
    l->c.rx_frames++;
}

static void uart_on_hello_frame(a20_clx_uart_link_t *l, const uint8_t *frame) {
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint32_t txid = a20_clx_get_le32(frame + A20_CLX_OFF_TXID);
    uint8_t peer_caps = 0;
    uint16_t short_addr;
    uint8_t ack_payload[A20_CLX_HELLO_LEN];
    uint8_t ack_frame[A20_CLX_HDR_LEN + A20_CLX_HELLO_LEN + A20_CLX_CRC_LEN];
    int rc, len;

    if (payload[A20_CLX_HELLO_OFF_PROTO_MAX] < A20_CLX_SESSION_VER &&
        l->peer_version == A20_CLX_SESSION_VER &&
        (l->state == A20_CLX_UART_L_UP || l->state == A20_CLX_UART_L_SUSPECT)) {
        l->c.rx_drops++;
        return;
    }
    rc = uart_validate_hello(l, frame, &peer_caps);
    if (rc == -ENOENT) {
        l->c.hello_rejects++;
        return;
    }
    if (rc != 0) {
        l->c.hello_rejects++;
        uart_send_hello_error(l, txid);
        uart_link_down(l, timer_get_ticks());
        return;
    }
    /* A dialing peer carries its current short address; 0xFFFF means it is
     * unassigned and takes the address we announce. */
    short_addr = a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR);
    if (short_addr == A20_CLX_UART_SHORT_UNASSIGNED)
        short_addr = l->assigned_addr;

    /* Answer with HELLO_ACK carrying our assignment (02 §6: the head node
     * assigns the leaf's short address in the HELLO exchange). */
    uart_fill_hello_payload(l, ack_payload, l->assigned_addr, l->nonce);
    len = a20_clx_build_frame(
        ack_frame, sizeof(ack_frame), A20_CLX_TYPE_HELLO_ACK, 0, 0, txid,
        a20_clx_fnv1a32(l->self_node_id),
        a20_clx_fnv1a32(payload + A20_CLX_HELLO_OFF_NODE_ID), 0, ack_payload,
        (uint16_t)A20_CLX_HELLO_LEN);
    if (len < 0) {
        l->c.tx_drops++;
        return;
    }
    if (uart_link_output(l, ack_frame, (uint32_t)len) < 0)
        return;
    if (payload[A20_CLX_HELLO_OFF_PROTO_MAX] >= A20_CLX_SESSION_VER) {
        int same = (l->state == A20_CLX_UART_L_UP ||
                    l->state == A20_CLX_UART_L_SUSPECT) &&
                   l->peer_nonce ==
                       a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE) &&
                   !memcmp(l->peer_node_id, payload, 16);
        if (!same && !l->hello_cookie)
            (void)uart_send_hello(l, timer_get_ticks());
    } else {
        l->peer_nonce = a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE);
        l->peer_caps = peer_caps;
        l->peer_version = A20_CLX_WIRE_VER;
        uart_link_up(l, short_addr, payload, timer_get_ticks());
    }
    l->c.rx_frames++;
}

static void uart_on_ping(a20_clx_uart_link_t *l, const uint8_t *frame) {
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint16_t payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    uint8_t frame_out[A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN];
    int len;

    /* Only a peer that completed the handshake may ping us; a PING from a
     * stranger is a drop (02 §6 machine: PING is an UP/SUSPECT-phase
     * heartbeat). */
    if (l->state != A20_CLX_UART_L_UP && l->state != A20_CLX_UART_L_SUSPECT) {
        l->c.rx_drops++;
        return;
    }
    if (payload_len != 8 || src_hash != a20_clx_fnv1a32(l->peer_node_id)) {
        l->c.rx_drops++;
        return;
    }
    /* 02 §2: PONG echoes the PING payload verbatim. */
    len = a20_clx_build_frame(frame_out, sizeof(frame_out), A20_CLX_TYPE_PONG,
                              0, 0, 0, a20_clx_fnv1a32(l->self_node_id),
                              a20_clx_fnv1a32(l->peer_node_id), 0, payload, 8);
    if (len < 0 || uart_link_output(l, frame_out, (uint32_t)len) != 0)
        return;
    l->c.rx_frames++;
}

static void uart_on_pong(a20_clx_uart_link_t *l, const uint8_t *frame) {
    const uint8_t *payload = frame + A20_CLX_OFF_PAYLOAD;
    uint16_t payload_len = a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN);
    uint32_t src_hash = a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH);
    uint64_t ts_us, now_us;
    int64_t sample;

    if (l->state != A20_CLX_UART_L_UP && l->state != A20_CLX_UART_L_SUSPECT) {
        l->c.rx_drops++;
        return;
    }
    if (payload_len != 8 || src_hash != a20_clx_fnv1a32(l->peer_node_id) ||
        !l->awaiting_pong) {
        l->c.rx_drops++;
        return;
    }
    ts_us = a20_clx_get_le32(payload) |
            ((uint64_t)a20_clx_get_le32(payload + 4) << 32);
    now_us = uart_now_us();
    if (ts_us > now_us) { /* peer clock skew: not an RTT sample */
        l->c.rx_drops++;
        return;
    }
    sample = (int64_t)(now_us - ts_us);
    if (sample < 0)
        sample = 0;
    /* 02 §6: sliding mean with alpha = 1/8. */
    l->rtt_us = (uint32_t)(((uint64_t)l->rtt_us * 7u + (uint64_t)sample) / 8u);
    l->awaiting_pong = 0;
    l->pings_missed = 0;
    if (l->state == A20_CLX_UART_L_SUSPECT) {
        l->state = A20_CLX_UART_L_UP; /* 02 §6: "SUSPECT --收PONG--> UP" */
        kinfo("cluster-uart: link %d SUSPECT->UP\n", l->id);
    }
    l->c.rx_frames++;
}

static void uart_on_frame(a20_clx_uart_link_t *l, const uint8_t *frame,
                          uint32_t len) {
    uint8_t type = frame[A20_CLX_OFF_TYPE];
    uint16_t flags = a20_clx_get_le16(frame + A20_CLX_OFF_FLAGS);

    if (uart_screen_frame(l, frame, len) != 0)
        return;

    /* 02 §3/§5: UART v0 has no RELIABLE peer (the leaf negotiates CAP_LEAF)
     * and fragmentation is forbidden outright (04 §5). */
    if (flags & (A20_CLX_FLAG_FRAGMENTED | A20_CLX_FLAG_BROADCAST)) {
        l->c.rx_drops++;
        return;
    }

    /* 02 §1: "ver 不匹配 -> 丢弃 + 计数；ver 高于本地时在 HELLO 阶段处理
     * (§6)，数据帧静默丢弃" -- data frames with a foreign ver are counted
     * here; HELLO-family frames carry their ver into the machine, where
     * the negotiation failure path answers with ERROR. */
    if (type != A20_CLX_TYPE_HELLO && type != A20_CLX_TYPE_HELLO_ACK &&
        frame[A20_CLX_OFF_VER] > l->peer_version) {
        l->c.rx_malformed++;
        return;
    }

    switch (type) {
        case A20_CLX_TYPE_HELLO:
            uart_on_hello_frame(l, frame);
            return;
        case A20_CLX_TYPE_HELLO_ACK:
            uart_on_ack_frame(l, frame);
            return;
        case A20_CLX_TYPE_PING:
            uart_on_ping(l, frame);
            return;
        case A20_CLX_TYPE_PONG:
            uart_on_pong(l, frame);
            return;
        case A20_CLX_TYPE_ACK:
        case A20_CLX_TYPE_NACK:
        case A20_CLX_TYPE_SEND:
        case A20_CLX_TYPE_CALL:
        case A20_CLX_TYPE_CALL_REPLY:
        case A20_CLX_TYPE_CLOSE:
        case A20_CLX_TYPE_ERROR:
            /* Data plane: hand the decoded frame to the cluster core (04 §1
         * upcall).  With nothing attached the frame is counted and gone. */
            if ((l->state != A20_CLX_UART_L_UP &&
                 l->state != A20_CLX_UART_L_SUSPECT) ||
                (a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH) !=
                     a20_clx_fnv1a32(l->peer_node_id) &&
                 !(l->peer_caps & A20_CLX_CAP_RELAY))) {
                l->c.rx_drops++;
                return;
            }
            if (l->rx_handler) {
                l->c.rx_frames++;
                uint8_t nh[2];
                a20_clx_put_le16(nh, l->peer_addr);
                l->rx_handler(l->rx_userdata, nh, 2, frame, len);
            } else {
                l->c.rx_drops++;
            }
            return;
        default:
            l->c.rx_malformed++; /* 02 §1: unknown type -> drop + count */
            return;
    }
}

/* ------------------------------------------------------------------ */
/* Timer-driven half of the machine                                    */
/* ------------------------------------------------------------------ */

static void uart_tick(a20_clx_uart_link_t *l, uint64_t now) {
    uint8_t ping_payload[8];
    uint8_t frame[A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN];
    uint64_t now_us;
    int len;

    if (l->hello_cookie &&
        (l->state == A20_CLX_UART_L_UP || l->state == A20_CLX_UART_L_SUSPECT) &&
        now - l->hello_sent_ticks >=
            MS_TO_TICKS(A20_CLX_UART_HELLO_TIMEOUT_MS)) {
        if (++l->hello_retries >= A20_CLX_UART_HELLO_RETRIES)
            l->hello_cookie = 0;
        else
            (void)uart_send_hello(l, now);
    }
    switch (l->state) {
        case A20_CLX_UART_L_DOWN:
            /* Redial after a quiet cooldown so a rebooted leaf is picked up
         * again (02 §6 re-negotiation, head-initiated because the leaf
         * never dials). */
            if (now - l->hello_sent_ticks >=
                MS_TO_TICKS(A20_CLX_UART_HELLO_TIMEOUT_MS))
                (void)uart_send_hello(l, now);
            return;

        case A20_CLX_UART_L_HELLO_SENT:
            if (now - l->hello_sent_ticks <
                MS_TO_TICKS(A20_CLX_UART_HELLO_TIMEOUT_MS))
                return;
            l->hello_retries++;
            if (l->hello_retries >= A20_CLX_UART_HELLO_RETRIES) {
                /* 02 §6: "HELLO_SENT --超时×3--> DOWN（上报 LINK_DOWN）". */
                uart_link_down(l, now);
                return;
            }
            (void)uart_send_hello(l, now);
            return;

        case A20_CLX_UART_L_UP:
        case A20_CLX_UART_L_SUSPECT:
            if (now - l->last_ping_ticks <
                MS_TO_TICKS(A20_CLX_UART_PING_PERIOD_MS))
                return;
            if (l->awaiting_pong) {
                l->pings_missed++;
                if (l->pings_missed >= 2 * A20_CLX_UART_HELLO_RETRIES) {
                    /* 02 §6: UP loses 3 -> SUSPECT, 3 more -> DOWN. */
                    uart_link_down(l, now);
                    return;
                }
                if (l->state == A20_CLX_UART_L_UP &&
                    l->pings_missed >= A20_CLX_UART_HELLO_RETRIES) {
                    l->state = A20_CLX_UART_L_SUSPECT;
                    kinfo("cluster-uart: link %d UP->SUSPECT\n", l->id);
                }
            }
            now_us = uart_now_us();
            l->ping_ts_us = now_us;
            a20_clx_put_le64(ping_payload, now_us);
            len = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_PING,
                                      0, 0, 0, a20_clx_fnv1a32(l->self_node_id),
                                      a20_clx_fnv1a32(l->peer_node_id), 0,
                                      ping_payload, 8);
            if (len >= 0 && uart_link_output(l, frame, (uint32_t)len) == 0)
                l->awaiting_pong = 1;
            l->last_ping_ticks = now;
            return;

        default:
            return;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int a20_clx_uart_link_register(const a20_clx_uart_link_cfg_t *cfg) {
    a20_clx_uart_link_t *l = NULL;
    uint64_t flags;
    uint8_t sum = 0;
    int i, id = -ENOSPC, j;

    if (!cfg || !cfg->drv.getc || !cfg->drv.putc)
        return -EINVAL;
    for (j = 0; j < (int)A20_CLX_NODE_ID_LEN; j++)
        sum |= cfg->self_node_id[j];
    if (sum == 0)
        return -EINVAL; /* LOCAL is not a wire identity (01-abi) */

    flags = spin_lock_irqsave(&g_uart_lock);
    for (i = 0; i < (int)A20_CLX_UART_MAX_LINKS; i++) {
        if (!g_uart_links[i].used) {
            l = &g_uart_links[i];
            id = i;
            break;
        }
    }
    if (!l) {
        spin_unlock_irqrestore(&g_uart_lock, flags);
        return -ENOSPC;
    }
    memset(l, 0, sizeof(*l));
    l->id = id;
    l->used = 1;
    memcpy(l->self_node_id, cfg->self_node_id, A20_CLX_NODE_ID_LEN);
    l->assigned_addr = cfg->assigned_addr;
    l->drv = cfg->drv;
    l->state = A20_CLX_UART_L_DOWN;
    l->nonce = cfg->session
                   ? cfg->session
                   : uart_next_nonce(a20_clx_fnv1a32(cfg->self_node_id));
    l->self_caps = cfg->caps;
    a20_clx_slip_rx_init(&l->rx, l->rx_buf, sizeof(l->rx_buf),
                         MS_TO_TICKS(A20_CLX_UART_INTERBYTE_MS));
    spin_unlock_irqrestore(&g_uart_lock, flags);
    return id;
}

int a20_clx_uart_link_unregister(int link_id) {
    uint64_t flags;
    int rc = -ENOENT;
    a20_clx_uart_link_event_fn event = NULL;
    void *event_ctx = NULL;
    uint8_t nh[2] = {0};

    if (link_id < 0 || link_id >= (int)A20_CLX_UART_MAX_LINKS)
        return -EINVAL;
    flags = spin_lock_irqsave(&g_uart_lock);
    if (g_uart_links[link_id].pumping) {
        rc = -EBUSY;
    } else if (g_uart_links[link_id].used) {
        a20_clx_uart_link_t *l = &g_uart_links[link_id];
        if (l->state == A20_CLX_UART_L_UP ||
            l->state == A20_CLX_UART_L_SUSPECT) {
            a20_clx_put_le16(nh, l->peer_addr);
            event = g_uart_event_fn;
            event_ctx = g_uart_event_ud;
        }
        memset(&g_uart_links[link_id], 0, sizeof(g_uart_links[link_id]));
        rc = 0;
    }
    spin_unlock_irqrestore(&g_uart_lock, flags);
    if (event)
        event(event_ctx, nh, 2, 0);
    return rc;
}

int a20_clx_uart_send_frame(const uint8_t *next_hop, uint32_t nh_len,
                            const uint8_t *frame, uint32_t len) {
    uint16_t addr;
    a20_clx_uart_link_t *l = NULL;
    uint64_t flags;
    int i, rc = -ENOENT;

    if (!next_hop || !frame || nh_len != 2)
        return -EINVAL;
    if (len < A20_CLX_HDR_LEN + A20_CLX_CRC_LEN || len > A20_CLX_UART_MTU)
        return -EMSGSIZE; /* 02 §5: over MTU on UART is invalid, not fragmented */
    addr = a20_clx_get_le16(next_hop);
    if (addr == A20_CLX_UART_SHORT_UNASSIGNED)
        return -ENOENT;

    flags = spin_lock_irqsave(&g_uart_lock);
    for (i = 0; i < (int)A20_CLX_UART_MAX_LINKS; i++) {
        a20_clx_uart_link_t *cand = &g_uart_links[i];
        if (cand->used && cand->published.state == A20_CLX_STATE_UP &&
            cand->published.peer_addr == addr) {
            l = cand;
            break;
        }
    }
    if (l && l->tx_count < 4) {
        uint32_t index = (l->tx_head + l->tx_count) % 4;
        memcpy(l->tx_queue[index], frame, len);
        l->tx_len[index] = (uint16_t)len;
        l->tx_count++;
        rc = 0;
    } else if (l) {
        rc = -EAGAIN;
    } else {
        g_uart_route_misses++;
    }
    spin_unlock_irqrestore(&g_uart_lock, flags);
    return rc;
}

void a20_clx_uart_poll_link(int link_id) {
    a20_clx_uart_link_t *l;
    uint64_t now;
    int c;

    if (link_id < 0 || link_id >= (int)A20_CLX_UART_MAX_LINKS)
        return;
    l = &g_uart_links[link_id];
    uint64_t guard = spin_lock_irqsave(&g_uart_lock);
    if (!l->used || l->pumping) {
        spin_unlock_irqrestore(&g_uart_lock, guard);
        return;
    }
    l->pumping = 1;
    l->rx_handler = g_uart_rx_fn;
    l->rx_userdata = g_uart_rx_ud;
    l->event_handler = g_uart_event_fn;
    l->event_userdata = g_uart_event_ud;
    spin_unlock_irqrestore(&g_uart_lock, guard);

    /*
     * The pump context owns every per-link field it touches below (rx
     * decoder, rx_* counters, state machine): the send path only reads
     * state and writes the tx_* counters, and register/unregister are
     * config-time operations that must not run against an active pump.
     * g_uart_lock therefore guards the table and the handler pointers, not
     * this loop -- the driver getc busy-waits and must run with IRQs on.
     */
    now = timer_get_ticks();
    uint32_t rx_budget = 128;
    while (rx_budget-- && (c = l->drv.getc(l->drv.ctx)) >= 0) {
        int r = a20_clx_slip_rx_byte(&l->rx, (uint8_t)c, now);
        if (r == 1)
            uart_on_frame(l, l->rx.buf, l->rx.len);
        else if (r < 0)
            l->c.rx_drops++; /* half frame discarded, decoder re-synced */
    }
    /* 04 §4: >50 ms of inter-byte silence kills the half frame. */
    if (a20_clx_slip_rx_timeout(&l->rx, now))
        l->c.rx_drops++;

    uart_tick(l, now);
    /* One byte-stream writer prevents CALL/control frame interleaving. */
    uint8_t tx[A20_CLX_UART_MTU];
    uint16_t tx_len = 0;
    uint64_t irq = spin_lock_irqsave(&g_uart_lock);
    if (l->tx_count) {
        tx_len = l->tx_len[l->tx_head];
        memcpy(tx, l->tx_queue[l->tx_head], tx_len);
        l->tx_head = (l->tx_head + 1) % 4;
        l->tx_count--;
    }
    spin_unlock_irqrestore(&g_uart_lock, irq);
    if (tx_len && l->state == A20_CLX_UART_L_UP)
        uart_link_output(l, tx, tx_len);
    irq = spin_lock_irqsave(&g_uart_lock);
    l->published.link_id = l->id;
    l->published.state = l->state == A20_CLX_UART_L_UP ? A20_CLX_STATE_UP
                         : l->state == A20_CLX_UART_L_SUSPECT
                             ? A20_CLX_STATE_SUSPECT
                             : A20_CLX_STATE_DOWN;
    l->published.assigned_addr = l->assigned_addr;
    l->published.peer_addr = l->peer_addr;
    l->published.rtt_us = l->rtt_us;
    l->published.last_hello_age_ms =
        (now - l->hello_done_ms) * 1000ULL / TICKS_PER_SEC;
    l->published.counters = l->c;
    l->published_local = l->nonce;
    l->published_remote = l->peer_nonce;
    l->published_caps = l->peer_version >= A20_CLX_SESSION_VER
                            ? l->peer_caps
                            : l->peer_caps & ~A20_CLX_CAP_RELIABLE;
    memcpy(l->published_node, l->peer_node_id, 16);
    l->pumping = 0;
    spin_unlock_irqrestore(&g_uart_lock, irq);
}

void a20_clx_uart_poll_all(void) {
    int i;

    for (i = 0; i < (int)A20_CLX_UART_MAX_LINKS; i++)
        a20_clx_uart_poll_link(i);
}

void a20_clx_uart_set_rx_handler(a20_clx_uart_rx_fn fn, void *ud) {
    uint64_t flags;

    flags = spin_lock_irqsave(&g_uart_lock);
    g_uart_rx_fn = fn;
    g_uart_rx_ud = ud;
    spin_unlock_irqrestore(&g_uart_lock, flags);
}

void a20_clx_uart_set_link_event_handler(a20_clx_uart_link_event_fn fn,
                                         void *ud) {
    uint64_t flags;

    flags = spin_lock_irqsave(&g_uart_lock);
    g_uart_event_fn = fn;
    g_uart_event_ud = ud;
    spin_unlock_irqrestore(&g_uart_lock, flags);
}

int a20_clx_uart_get_status(int link_id, a20_clx_uart_link_status_t *out) {
    a20_clx_uart_link_t *l;
    uint64_t flags;
    int rc = -ENOENT;

    if (link_id < 0 || link_id >= (int)A20_CLX_UART_MAX_LINKS || !out)
        return -EINVAL;
    flags = spin_lock_irqsave(&g_uart_lock);
    l = &g_uart_links[link_id];
    if (l->used) {
        *out = l->published;
        rc = 0;
    }
    spin_unlock_irqrestore(&g_uart_lock, flags);
    return rc;
}

int a20_clx_uart_session(const uint8_t *nh, uint32_t nh_len,
                         const uint8_t node[16], uint64_t *local,
                         uint64_t *remote, uint32_t *caps) {
    if (!nh || nh_len != 2)
        return -EINVAL;
    int result = -ENOENT;
    uint64_t irq = spin_lock_irqsave(&g_uart_lock);
    for (uint32_t i = 0; i < A20_CLX_UART_MAX_LINKS; i++) {
        a20_clx_uart_link_t *l = &g_uart_links[i];
        if (l->used && l->published.state == A20_CLX_STATE_UP &&
            l->published.peer_addr == a20_clx_get_le16(nh) &&
            memcmp(l->published_node, node, 16) == 0) {
            *local = l->published_local;
            *remote = l->published_remote;
            *caps = l->published_caps;
            result = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&g_uart_lock, irq);
    return result;
}

uint64_t a20_clx_uart_route_misses(void) {
    return g_uart_route_misses;
}

#else /* CONFIG_CLUSTER_PROFILE < DEFAULT */

/* The MCU tier carries the leaf (kernel/mcu/leaf.c) and needs none of the
 * head machine; keep a non-empty translation unit (ISO C) and a visible
 * reason for the file to exist in that configuration. */
typedef int a20_clx_uart_headless_guard;

#endif /* CONFIG_CLUSTER_PROFILE >= DEFAULT */
