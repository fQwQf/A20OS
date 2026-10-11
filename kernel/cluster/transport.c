/* Cluster adapter registry and bounded receive dispatch.
 * RX, polling and housekeeping run independently. Driver I/O and channel
 * injection never run under the registry lock. Valid nonlocal frames may
 * be forwarded only by a RELAY, with TTL/CRC updated in the shared codec.
 * See docs/cluster/03-kernel-impl.md and 04-transports.md. */
#include "cluster/clx_internal.h"
#include "cluster/loopback.h"
#include "cluster/udp.h"
#include "cluster/fragment.h"

#include "core/klog.h"
#include "core/random.h"
#include "mm/slab.h"
#include "proc/proc.h"

#define CLX_RX_RING 64 /* queued frame pointers, not bytes */

typedef struct clx_rx_item {
    uint8_t *frame;
    uint32_t len;
    uint32_t transport_id;
    uint8_t nh_src[A20_CLX_LINK_ADDR_MAX];
    uint32_t nh_len;
} clx_rx_item_t;

static a20_clx_transport_t g_clx_transports[A20_CLX_MAX_TRANSPORTS];
static uint32_t g_clx_transport_count;

static spinlock_t g_clx_rx_lock = SPINLOCK_INIT;
static clx_rx_item_t g_clx_rx_ring[CLX_RX_RING];
static uint32_t g_clx_rx_head, g_clx_rx_tail;
static uint32_t g_clx_rx_bytes, g_clx_rx_peak_bytes;
static int g_clx_core_ready;
static uint64_t g_clx_loop_session;

uint64_t a20_clx_boot_session(void) {
    return g_clx_loop_session;
}

/* Per-link counters (02-§10), keyed (transport_id, next_hop). */
typedef struct clx_link_stats {
    int in_use;
    uint32_t transport_id;
    uint8_t next_hop[A20_CLX_LINK_ADDR_MAX];
    uint32_t nh_len;
    uint32_t state; /* A20_CLX_LINK_* */
    uint64_t tx_frames, rx_frames, tx_drops, rx_drops, rx_malformed;
    uint64_t retransmits;
    uint64_t last_hello_tick;
} clx_link_stats_t;
static clx_link_stats_t g_clx_links[CLX_LIMIT_ROUTES + 4];
static spinlock_t g_clx_links_lock = SPINLOCK_INIT;

/* ---- link counters ------------------------------------------------------ */

static clx_link_stats_t *clx_link_find(uint32_t transport_id, const uint8_t *nh,
                                       uint32_t nh_len, int create) {
    clx_link_stats_t *free_slot = NULL;

    for (uint32_t i = 0; i < sizeof(g_clx_links) / sizeof(g_clx_links[0]);
         i++) {
        clx_link_stats_t *l = &g_clx_links[i];
        if (!l->in_use) {
            if (!free_slot)
                free_slot = l;
            continue;
        }
        if (l->transport_id == transport_id && l->nh_len == nh_len &&
            (nh_len == 0 || memcmp(l->next_hop, nh, nh_len) == 0))
            return l;
    }
    if (!create || !free_slot)
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = 1;
    free_slot->transport_id = transport_id;
    free_slot->state = A20_CLX_LINK_DOWN;
    if (nh && nh_len <= A20_CLX_LINK_ADDR_MAX) {
        memcpy(free_slot->next_hop, nh, nh_len);
        free_slot->nh_len = nh_len;
    }
    return free_slot;
}

void a20_clx_link_rx_drop(uint32_t transport_id, const uint8_t *nh,
                          uint32_t nh_len) {
    uint64_t flags = spin_lock_irqsave(&g_clx_links_lock);
    clx_link_stats_t *l = clx_link_find(transport_id, nh, nh_len, 1);
    if (l)
        l->rx_drops++;
    spin_unlock_irqrestore(&g_clx_links_lock, flags);
}

void a20_clx_link_rx_malformed(uint32_t transport_id, const uint8_t *nh,
                               uint32_t nh_len) {
    uint64_t flags = spin_lock_irqsave(&g_clx_links_lock);
    clx_link_stats_t *l = clx_link_find(transport_id, nh, nh_len, 1);
    if (l)
        l->rx_malformed++;
    spin_unlock_irqrestore(&g_clx_links_lock, flags);
}

void a20_clx_link_tx_drop(uint32_t transport_id, const uint8_t *nh,
                          uint32_t nh_len) {
    uint64_t flags = spin_lock_irqsave(&g_clx_links_lock);
    clx_link_stats_t *l = clx_link_find(transport_id, nh, nh_len, 1);
    if (l)
        l->tx_drops++;
    spin_unlock_irqrestore(&g_clx_links_lock, flags);
}

/* ---- registry ------------------------------------------------------------ */

int a20_clx_transport_register(const a20_clx_transport_t *t) {
    if (!t || !t->send || t->mtu < A20_CLX_HDR_LEN + A20_CLX_CRC_LEN ||
        t->mtu > 65536u)
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    for (uint32_t i = 0; i < g_clx_transport_count; i++) {
        if (g_clx_transports[i].transport_id == t->transport_id) {
            spin_unlock_irqrestore(&g_clx_lock, flags);
            return -1;
        }
    }
    if (g_clx_transport_count >= A20_CLX_MAX_TRANSPORTS) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return -1;
    }
    g_clx_transports[g_clx_transport_count++] = *t;
    spin_unlock_irqrestore(&g_clx_lock, flags);
    return 0;
}

const a20_clx_transport_t *a20_clx_transport_get(uint32_t transport_id) {
    for (uint32_t i = 0; i < g_clx_transport_count; i++)
        if (g_clx_transports[i].transport_id == transport_id)
            return &g_clx_transports[i];
    return NULL;
}

uint32_t a20_clx_transport_mtu(uint32_t transport_id) {
    const a20_clx_transport_t *t = a20_clx_transport_get(transport_id);
    return t ? t->mtu : 0;
}

/* ---- TX: node -> route -> transport -> wire ------------------------------ */

static int clx_wire_send(const a20_clx_transport_t *t, uint32_t transport_id,
                         const uint8_t *nh, uint32_t nh_len,
                         const uint8_t *frame, uint32_t len) {
    int r = t->send(t->ctx, nh, nh_len, frame, len);
    if (r == 0) {
        uint64_t flags = spin_lock_irqsave(&g_clx_links_lock);
        clx_link_stats_t *l = clx_link_find(transport_id, nh, nh_len, 1);
        if (l) {
            l->tx_frames++;
            if (transport_id == A20_CLX_TRANSPORT_LOOPBACK &&
                l->state == A20_CLX_LINK_DOWN)
                l->state = A20_CLX_LINK_UP; /* 04-§5 loopback: 常 UP */
        }
        spin_unlock_irqrestore(&g_clx_links_lock, flags);
    } else {
        a20_clx_link_tx_drop(transport_id, nh, nh_len);
    }
    return r;
}

int a20_clx_core_send_frame(const a20_node_id_t *dst, const uint8_t *frame,
                            uint32_t len) {
    uint32_t transport_id, nh_len;
    uint8_t nh[A20_CLX_LINK_ADDR_MAX];
    const a20_clx_transport_t *t;
    int r;

    if (a20_clx_route_lookup(dst, &transport_id, nh, &nh_len) < 0) {
        a20_clx_link_tx_drop(0, NULL, 0);
        return -A20_ERR_NODE_UNREACHABLE;
    }
    t = a20_clx_transport_get(transport_id);
    if (!t) {
        a20_clx_link_tx_drop(transport_id, nh, nh_len);
        return -A20_ERR_NODE_UNREACHABLE;
    }
    if (len <= t->mtu)
        return clx_wire_send(t, transport_id, nh, nh_len, frame, len);
    uint32_t logical_mtu = a20_clx_logical_mtu(dst);
    a20_frame_hdr_t h;
    const uint8_t *payload;
    uint64_t local, remote;
    uint32_t caps;
    if (logical_mtu <= t->mtu || len > logical_mtu ||
        a20_frame_decode_logical(frame, len, logical_mtu, &h, &payload) !=
            A20_FRAME_OK ||
        h.flags != A20_CLX_FLAG_RELIABLE ||
        (h.type != A20_CLX_TYPE_CALL && h.type != A20_CLX_TYPE_CALL_REPLY) ||
        a20_clx_peer_session(dst, &local, &remote, &caps) < 0 ||
        !a20_clx_session_matches(payload, h.payload_len, local, remote)) {
        a20_clx_link_tx_drop(transport_id, nh, nh_len);
        return -A20_ERR_NO_SPACE;
    }
    uint8_t *fragment = kmalloc(t->mtu);
    if (!fragment)
        return -A20_ERR_NO_MEMORY;
    uint32_t chunk = a20_frame_max_payload(t->mtu) - A20_CLX_FRAGMENT_PREFIX;
    uint32_t body = h.payload_len - A20_CLX_SESSION_BYTES;
    uint32_t total = h.payload_len;
    uint8_t *part = fragment + A20_CLX_HDR_LEN;
    h.flags |= A20_CLX_FLAG_FRAGMENTED;
    r = 0;
    for (uint32_t offset = 0, index = 0; offset < body;
         offset += chunk, index++) {
        uint32_t length = body - offset < chunk ? body - offset : chunk;
        h.frag = index | (offset + length == body ? A20_CLX_FRAG_LAST : 0);
        h.payload_len = A20_CLX_FRAGMENT_PREFIX + length;
        memcpy(part, payload, A20_CLX_SESSION_BYTES);
        a20_clx_put_le32(part + 16, total);
        a20_clx_put_le32(part + 20, offset);
        memcpy(part + A20_CLX_FRAGMENT_PREFIX,
               payload + A20_CLX_SESSION_BYTES + offset, length);
        int n = a20_frame_encode(fragment, t->mtu, t->mtu, &h, part);
        if (n < 0 ||
            (r = clx_wire_send(t, transport_id, nh, nh_len, fragment, n)) < 0) {
            if (n < 0)
                r = -A20_ERR_INVALID_ARGUMENT;
            break;
        }
        /* A 64 KiB burst must not monopolize the network lane or fill a
         * peer's bounded datagram queues before its polling owner can run. */
        if ((index + 1) % 4 == 0 && offset + length < body)
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(1));
    }
    kfree(fragment);
    return r;
}

int a20_clx_peer_session(const a20_node_id_t *node, uint64_t *local,
                         uint64_t *remote, uint32_t *caps) {
    uint32_t id, nh_len;
    uint8_t nh[A20_CLX_LINK_ADDR_MAX];
    if (a20_clx_route_lookup(node, &id, nh, &nh_len) < 0)
        return -A20_ERR_NODE_UNREACHABLE;
    if (id == A20_CLX_TRANSPORT_LOOPBACK) {
        *local = *remote = g_clx_loop_session;
        *caps = A20_CLX_CAP_RELIABLE;
        return 0;
    }
    const a20_clx_transport_t *t = a20_clx_transport_get(id);
    return t && t->session ? t->session(t->ctx, nh, nh_len, node->bytes, local,
                                        remote, caps)
                           : -A20_ERR_CLUSTER_UNSUPPORTED;
}

void a20_clx_link_retransmit(const a20_node_id_t *node) {
    uint32_t transport, nh_len;
    uint8_t nh[16];
    if (a20_clx_route_lookup(node, &transport, nh, &nh_len) < 0)
        return;
    uint64_t flags = spin_lock_irqsave(&g_clx_links_lock);
    clx_link_stats_t *l = clx_link_find(transport, nh, nh_len, 1);
    if (l)
        l->retransmits++;
    spin_unlock_irqrestore(&g_clx_links_lock, flags);
}

/* ---- RX ring -------------------------------------------------------------- */

void a20_clx_rx_frame(uint32_t transport_id, const uint8_t *next_hop_src,
                      uint32_t nh_len, const uint8_t *frame, uint32_t len) {
    uint8_t *copy;

    uint32_t mtu = a20_clx_transport_mtu(transport_id);
    if (!frame || (!next_hop_src && nh_len) || nh_len > A20_CLX_LINK_ADDR_MAX ||
        len < A20_CLX_HDR_LEN || len > mtu)
        return;
    /* Copy here: the caller's buffer (loopback bounce buffer, UDP pbuf,
     * UART decode window) is only valid during the callback (04-§1 send =
     * 拷贝，不共享缓冲). */
    copy = kmalloc(len);
    if (!copy) {
        a20_clx_link_rx_drop(transport_id, next_hop_src, nh_len);
        return;
    }
    memcpy(copy, frame, len);

    uint64_t flags = spin_lock_irqsave(&g_clx_rx_lock);
    uint32_t next = (g_clx_rx_head + 1) % CLX_RX_RING;
    if (next == g_clx_rx_tail) {
        spin_unlock_irqrestore(&g_clx_rx_lock, flags);
        kfree(copy);
        a20_clx_link_rx_drop(transport_id, next_hop_src, nh_len);
        return;
    }
    clx_rx_item_t *it = &g_clx_rx_ring[g_clx_rx_head];
    it->frame = copy;
    it->len = len;
    it->transport_id = transport_id;
    it->nh_len = nh_len;
    memcpy(it->nh_src, next_hop_src, nh_len);
    g_clx_rx_head = next;
    g_clx_rx_bytes += len;
    if (g_clx_rx_bytes > g_clx_rx_peak_bytes)
        g_clx_rx_peak_bytes = g_clx_rx_bytes;
    spin_unlock_irqrestore(&g_clx_rx_lock, flags);
}

static int clx_rx_pop(clx_rx_item_t *out) {
    uint64_t flags = spin_lock_irqsave(&g_clx_rx_lock);

    if (g_clx_rx_head == g_clx_rx_tail) {
        spin_unlock_irqrestore(&g_clx_rx_lock, flags);
        return 0;
    }
    *out = g_clx_rx_ring[g_clx_rx_tail];
    g_clx_rx_bytes -= out->len;
    g_clx_rx_tail = (g_clx_rx_tail + 1) % CLX_RX_RING;
    spin_unlock_irqrestore(&g_clx_rx_lock, flags);
    return 1;
}

/* ---- RX thread: screen -> resolve -> dispatch ----------------------------- */

static void clx_dispatch(uint32_t transport_id, const uint8_t *nh_src,
                         uint32_t nh_len, uint8_t *frame, uint32_t len) {
    a20_frame_hdr_t h;
    const uint8_t *payload = NULL;
    a20_node_id_t src, dst, self;
    uint32_t mtu = a20_clx_transport_mtu(transport_id);

    /* Layer 1 (02-§1 hard rules).  A-12: wire-format violations count as
     * rx_malformed.  ttl==0 arrives exhausted (A-13) -> rx_drops. */
    a20_frame_verdict_t v =
        a20_frame_decode_session(frame, len, mtu, &h, &payload);
    if (v != A20_FRAME_OK) {
        a20_clx_link_rx_malformed(transport_id, nh_src, nh_len);
        return;
    }
    if (h.ttl == 0) {
        a20_clx_link_rx_drop(transport_id, nh_src, nh_len);
        return;
    }

    /* Identity resolution (04-§2 实现期记录): on loopback both src_hash
     * and dst_hash must resolve to hosted identities -- self or a routed
     * virtual node; one export namespace per kernel.  Link-address pinning
     * (anti-spoof) is a real-transport concern (UDP/UART); virtual-node
     * replies are emitted on the local node's link address, so the addr
     * cannot pin the sender here.  RELAY is v0-unimplemented: frames for
     * anyone else are dropped (rx_drops), never forwarded. */
    if (a20_clx_node_by_hash(h.src_hash, &src) < 0 ||
        a20_clx_self_get(&self, NULL) < 0) {
        a20_clx_link_rx_drop(transport_id, nh_src, nh_len);
        return;
    }
    if (a20_clx_node_by_hash(h.dst_hash, &dst) < 0) {
        a20_clx_link_rx_drop(transport_id, nh_src, nh_len);
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_clx_links_lock);
    clx_link_stats_t *l = clx_link_find(transport_id, nh_src, nh_len, 1);
    if (l)
        l->rx_frames++;
    spin_unlock_irqrestore(&g_clx_links_lock, flags);

    uint32_t dst_transport, dst_nh_len, caps;
    uint8_t dst_nh[A20_CLX_LINK_ADDR_MAX];
    int routed =
        a20_clx_route_lookup(&dst, &dst_transport, dst_nh, &dst_nh_len);
    int hosted = memcmp(dst.bytes, self.bytes, 16) == 0 ||
                 (routed == 0 && dst_transport == A20_CLX_TRANSPORT_LOOPBACK);
    if (!hosted) {
        a20_clx_self_get(NULL, &caps);
        uint32_t outgoing_mtu =
            routed == 0 ? a20_clx_transport_mtu(dst_transport) : 0;
        if (!(caps & A20_CLUSTER_CAP_RELAY) || !outgoing_mtu ||
            a20_frame_forward(frame, len, outgoing_mtu) < 0 ||
            a20_clx_core_send_frame(&dst, frame, len) < 0)
            a20_clx_link_rx_drop(transport_id, nh_src, nh_len);
        return;
    }

    /* Handlers take g_clx_lock internally and sleep only after releasing
     * it (03-§3 lock order: core lock -> channel locks).  An inbound CALL
     * is a request for one of our exports and goes to the export table
     * ONLY -- feeding it to the proxy demux let a CALL whose (src_hash,
     * txid) collided with a local proxy's in-flight transaction (which is
     * exactly what a connect to one's own node id produces on loopback)
     * tear that proxy down.  CLOSE is delivered to both halves: a proxy
     * learns its remote service closed, an export learns a caller left
     * (02-§8; the slot names the export endpoint from the sender's side in
     * both directions). */
    uint8_t *assembled = NULL;
    if (h.flags & A20_CLX_FLAG_FRAGMENTED) {
        assembled = a20_clx_fragment_rx(&src, transport_id, &h, payload);
        if (!assembled)
            return;
        payload = assembled;
    }
    if (a20_clx_reliable_rx(&src, &h, &payload)) {
        kfree(assembled);
        return;
    }
    if (h.type == A20_CLX_TYPE_CALL_REPLY || h.type == A20_CLX_TYPE_ERROR ||
        h.type == A20_CLX_TYPE_ACK) {
        a20_clx_proxy_rx(transport_id, &src, &h, payload);
    } else if (h.type == A20_CLX_TYPE_CALL) {
        a20_clx_export_rx(transport_id, &src, &h, payload);
    } else if (h.type == A20_CLX_TYPE_CLOSE) {
        if (h.txid != A20_CLX_CLOSE_CALLER)
            a20_clx_proxy_rx(transport_id, &src, &h, payload);
        a20_clx_export_rx(transport_id, &src, &h, payload);
    } else {
        /* HELLO/PING/PONG/ACK/NACK/SEND: loopback keeps no link state and
         * v0 emits none of these (04-§5: loopback heartbeat 无). */
        a20_clx_link_rx_drop(transport_id, nh_src, nh_len);
    }
    kfree(assembled);
}

static void clx_rx_thread(void) {
    proc_set_name(proc_current(), "clx-rx");
    for (;;) {
        if (__atomic_load_n(&g_clx_core_ready, __ATOMIC_ACQUIRE) == 3)
            return;
        a20_clx_fragment_expire();
        clx_rx_item_t it;
        if (clx_rx_pop(&it)) {
            clx_dispatch(it.transport_id, it.nh_src, it.nh_len, it.frame,
                         it.len);
            kfree(it.frame);
            continue;
        }
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(1));
    }
}

/* Drivers must progress even while RX dispatch waits for a service queue. */
static void clx_poll_thread(void) {
    proc_set_name(proc_current(), "clx-poll");
    for (;;) {
        if (__atomic_load_n(&g_clx_core_ready, __ATOMIC_ACQUIRE) == 3)
            return;
        for (uint32_t i = 0; i < A20_CLX_MAX_TRANSPORTS; i++) {
            a20_clx_transport_t t = {0};
            uint64_t flags = spin_lock_irqsave(&g_clx_lock);
            int present = i < g_clx_transport_count;
            if (present)
                t = g_clx_transports[i];
            spin_unlock_irqrestore(&g_clx_lock, flags);
            if (present && (t.flags & A20_CLX_TFL_POLLING) && t.poll)
                t.poll(t.ctx);
        }
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(1));
    }
}

static void clx_hk_thread(void) {
    proc_set_name(proc_current(), "clx-hk");
    for (;;) {
        if (__atomic_load_n(&g_clx_core_ready, __ATOMIC_ACQUIRE) == 3)
            return;
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(250));
        a20_clx_deadline_scan();
    }
}

/* ---- UART adapter (04-§4 note 4: WA wraps WC1's head node) ---------------- */

#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_DEFAULT
static void clx_uart_rx_adapter(void *ud, const uint8_t *nh, uint32_t nh_len,
                                const uint8_t *frame, uint32_t len) {
    (void)ud;
    a20_clx_rx_frame(A20_CLX_TRANSPORT_UART, nh, nh_len, frame, len);
}

static void clx_uart_link_adapter(void *ud, const uint8_t *nh, uint32_t nh_len,
                                  int up) {
    (void)ud;
    a20_clx_link_event(A20_CLX_TRANSPORT_UART, nh, nh_len, up);
}

static int clx_uart_send_wrap(void *ctx, const uint8_t *nh, uint32_t nh_len,
                              const void *frame, uint32_t len) {
    (void)ctx;
    return a20_clx_uart_send_frame(nh, nh_len, frame, len);
}

static int clx_uart_session_wrap(void *ctx, const uint8_t *nh, uint32_t len,
                                 const uint8_t node[16], uint64_t *local,
                                 uint64_t *remote, uint32_t *caps) {
    (void)ctx;
    return a20_clx_uart_session(nh, len, node, local, remote, caps);
}

static void clx_uart_poll_wrap(void *ctx) {
    (void)ctx;
    a20_clx_uart_poll_all();
}
#endif

/* ---- init ------------------------------------------------------------------ */

int a20_clx_core_init(void) {
    for (;;) {
        int state = __atomic_load_n(&g_clx_core_ready, __ATOMIC_ACQUIRE);
        if (state == 2)
            return 0;
        if (state == 3)
            return -A20_ERR_NO_MEMORY;
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_clx_core_ready, &expected, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(1));
    }

    g_clx_loop_session = random_u64() | 1ULL;
    a20_clx_loopback_init();
    /* loopback (04-§2): every tier, zero-loss, link addrs = 4B vnode no. */
    static a20_clx_transport_t clx_loopback_transport;
    memset(&clx_loopback_transport, 0, sizeof(clx_loopback_transport));
    clx_loopback_transport.transport_id = A20_CLX_TRANSPORT_LOOPBACK;
    clx_loopback_transport.mtu = 65536; /* 04-§5 */
    clx_loopback_transport.send = a20_clx_loopback_send;
    a20_clx_transport_register(&clx_loopback_transport);

#ifndef CONFIG_MCU
    a20_clx_udp_init();
#endif
#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_DEFAULT
    /* 04-§4 note 4: WA wraps WC1's head-node UART transport into the
     * 04-§1 contract.  Without registered links it stays inert. */
    static a20_clx_transport_t clx_uart_transport;
    memset(&clx_uart_transport, 0, sizeof(clx_uart_transport));
    clx_uart_transport.transport_id = A20_CLX_TRANSPORT_UART;
    clx_uart_transport.mtu = A20_CLX_UART_MTU;
    clx_uart_transport.flags =
        A20_CLX_TFL_POLLING | A20_CLX_TFL_RELIABLE_CAPABLE;
    clx_uart_transport.send = clx_uart_send_wrap;
    clx_uart_transport.poll = clx_uart_poll_wrap;
    clx_uart_transport.session = clx_uart_session_wrap;
    a20_clx_transport_register(&clx_uart_transport);
    a20_clx_uart_set_rx_handler(clx_uart_rx_adapter, NULL);
    a20_clx_uart_set_link_event_handler(clx_uart_link_adapter, NULL);
#endif

    if (proc_alloc_detached(clx_rx_thread) < 0 ||
        proc_alloc_detached(clx_hk_thread) < 0 ||
        proc_alloc_detached(clx_poll_thread) < 0) {
        __atomic_store_n(&g_clx_core_ready, 3, __ATOMIC_RELEASE);
        printf("[CLX] core thread spawn failed\n");
        return -A20_ERR_NO_MEMORY;
    }
    __atomic_store_n(&g_clx_core_ready, 2, __ATOMIC_RELEASE);
    printf("[CLX] core ready transports=%u\n", g_clx_transport_count);
    return 0;
}

/* ---- link_status fill (01-abi §6, 02-§10) ---------------------------------- */

int a20_clx_link_status_fill(const a20_node_id_t *node, uint32_t *state,
                             uint32_t *rtt_us, uint64_t *tx_frames,
                             uint64_t *rx_frames, uint64_t *tx_drops,
                             uint64_t *rx_drops, uint64_t *retransmits,
                             uint64_t *last_hello_age_ms) {
    a20_node_id_t self;
    uint32_t transport_id, nh_len;
    uint8_t nh[A20_CLX_LINK_ADDR_MAX];
    uint64_t flags;

    *rtt_us = 0; /* MCU 档恒 0; loopback 无 RTT 采样 */
    *last_hello_age_ms = 0;
    *retransmits = 0;

    if (a20_clx_self_get(&self, NULL) < 0)
        return -A20_ERR_NODE_UNREACHABLE;

    /* A20_NODE_ID_LOCAL (all-zero, 01-abi §节点标识) is the aggregation
     * selector, not a routable node -- comparing it against self always
     * failed and fell through to route_lookup, which made link_status(LOCAL)
     * unconditionally NODE_UNREACHABLE. */
    int is_local = 1;
    for (int i = 0; i < 16; i++)
        if (node->bytes[i]) {
            is_local = 0;
            break;
        }

    if (is_local || memcmp(self.bytes, node->bytes, 16) == 0) {
        /* LOCAL aggregates every link (01-abi §6). */
        flags = spin_lock_irqsave(&g_clx_links_lock);
        *tx_frames = *rx_frames = *tx_drops = *rx_drops = 0;
        *state = A20_CLX_LINK_DOWN;
        for (uint32_t i = 0; i < sizeof(g_clx_links) / sizeof(g_clx_links[0]);
             i++) {
            clx_link_stats_t *l = &g_clx_links[i];
            if (!l->in_use)
                continue;
            *tx_frames += l->tx_frames;
            *rx_frames += l->rx_frames;
            *tx_drops += l->tx_drops;
            *rx_drops += l->rx_drops;
            if (l->state == A20_CLX_LINK_UP)
                *state = A20_CLX_LINK_UP;
        }
        spin_unlock_irqrestore(&g_clx_links_lock, flags);
        return 0;
    }

    if (a20_clx_route_lookup(node, &transport_id, nh, &nh_len) < 0)
        return -A20_ERR_NODE_UNREACHABLE;
    flags = spin_lock_irqsave(&g_clx_links_lock);
    clx_link_stats_t *l = clx_link_find(transport_id, nh, nh_len, 0);
    if (l) {
        *state = l->state;
        *tx_frames = l->tx_frames;
        *rx_frames = l->rx_frames;
        *tx_drops = l->tx_drops;
        *rx_drops = l->rx_drops;
        *retransmits = l->retransmits;
        *last_hello_age_ms = l->last_hello_tick
                                 ? (timer_get_ticks() - l->last_hello_tick) /
                                       (TICKS_PER_SEC / 1000)
                                 : 0;
    } else {
        /* Route exists but the link has no traffic yet: loopback links are
         * UP by construction (04-§5). */
        *state = transport_id == A20_CLX_TRANSPORT_LOOPBACK ? A20_CLX_LINK_UP
                                                            : A20_CLX_LINK_DOWN;
        *tx_frames = *rx_frames = *tx_drops = *rx_drops = 0;
    }
    spin_unlock_irqrestore(&g_clx_links_lock, flags);
    return 0;
}

/* ---- link events (04-§1 上行) ---------------------------------------------- */

void a20_clx_link_event(uint32_t transport_id, const uint8_t *next_hop,
                        uint32_t nh_len, int up) {
    uint64_t flags = spin_lock_irqsave(&g_clx_links_lock);
    clx_link_stats_t *l = clx_link_find(transport_id, next_hop, nh_len, 1);

    if (l)
        l->state = up ? A20_CLX_LINK_UP : A20_CLX_LINK_DOWN;
    spin_unlock_irqrestore(&g_clx_links_lock, flags);

    a20_clx_event_push(up ? A20_CLX_EV_LINK_UP : A20_CLX_EV_LINK_DOWN, 0,
                       transport_id, 0);
    if (!up)
        a20_clx_route_link_down(transport_id, next_hop, nh_len);
}

/* ---- events (01-abi §5) ----------------------------------------------------- */

typedef struct clx_sub {
    int in_use;
    uint64_t mask; /* bit (A20_CLX_EV_BASE + kind) */
    a20_eventq_t *eq;
} clx_sub_t;
static clx_sub_t g_clx_subs[8];
static uint64_t g_clx_event_overflows;
static const uint32_t g_clx_event_src; /* token object, never dereferenced */

a20_eventq_t *a20_clx_event_subscribe(uint32_t mask) {
    uint64_t want = 0;

    for (uint32_t k = 0; k < 4; k++)
        if (mask & (1u << k))
            want |= (uint64_t)1 << (A20_CLX_EV_BASE + k);

    a20_eventq_t *eq = a20_eventq_create(A20_EVQ_DEFAULT_CAP);
    if (!eq)
        return NULL;
    /* Watch the cluster token object: a20_event_notify() matches watches
     * by (object, type) and appends to the owner queue. */
    if (a20_eventq_watch(eq, 0, (void *)&g_clx_event_src, A20_OBJ_DEBUG, want,
                         0, 0) < 0) {
        a20_eventq_release(eq);
        return NULL;
    }
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    clx_sub_t *s = NULL;
    for (uint32_t i = 0; i < sizeof(g_clx_subs) / sizeof(g_clx_subs[0]); i++)
        if (!g_clx_subs[i].in_use) {
            s = &g_clx_subs[i];
            break;
        }
    if (s) {
        s->in_use = 1;
        s->mask = want;
        s->eq = eq;
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (!s) {
        a20_eventq_release(eq);
        return NULL;
    }
    return eq;
}

void a20_clx_event_push(uint32_t kind, uint32_t node_hash,
                        uint32_t transport_id, uint32_t reason) {
    uint32_t ev = A20_CLX_EV_BASE + kind;
    uint64_t data0 = node_hash;
    uint64_t data1 = transport_id | ((uint64_t)reason << 8);

    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    for (uint32_t i = 0; i < sizeof(g_clx_subs) / sizeof(g_clx_subs[0]); i++) {
        clx_sub_t *s = &g_clx_subs[i];
        if (!s->in_use || !(s->mask & ((uint64_t)1 << ev)))
            continue;
        /* 01-abi §5: 成员事件允许丢失。ring 满时 eventq 的 wake-then-keep
         * 语义丢新事件；溢出计数经 klog 暴露（link_status 输出结构无此
         * 字段，01-abi 落地状态已登记）。 */
        uint64_t eqflags = spin_lock_irqsave(&s->eq->lock);
        int full = (s->eq->ring_count >= s->eq->ring_cap);
        spin_unlock_irqrestore(&s->eq->lock, eqflags);
        a20_event_notify((void *)&g_clx_event_src, A20_OBJ_DEBUG, ev, data0,
                         data1);
        if (full)
            g_clx_event_overflows++;
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
}

uint64_t a20_clx_event_overflows(void) {
    return g_clx_event_overflows;
}

/* Prepare the selected board stream before a route becomes visible. */
int a20_clx_uart_prepare_route(const uint8_t *nh, uint32_t len) {
#ifdef CONFIG_CLUSTER_UART_COM2
    static int prepared;
    if (len != 2 || a20_clx_get_le16(nh) != 1)
        return -A20_ERR_INVALID_ARGUMENT;
    if (__atomic_load_n(&prepared, __ATOMIC_ACQUIRE) == 2)
        return 0;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&prepared, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -A20_ERR_BUSY;
    a20_clx_uart_link_cfg_t cfg = {0};
    a20_node_id_t self;
    int result = -A20_ERR_NODE_UNREACHABLE;
    extern int arch_cluster_uart_com2_init(a20_clx_uart_drv_t *);
    if (a20_clx_self_get(&self, &cfg.caps) == 0 &&
        arch_cluster_uart_com2_init(&cfg.drv) == 0) {
        memcpy(cfg.self_node_id, self.bytes, 16);
        cfg.session = a20_clx_boot_session();
        cfg.assigned_addr = 1;
        result = a20_clx_uart_link_register(&cfg) >= 0 ? 0 : -A20_ERR_NO_SPACE;
    }
    __atomic_store_n(&prepared, result ? 0 : 2, __ATOMIC_RELEASE);
    return result;
#else
    (void)nh;
    (void)len;
    /* Other boards can register links through the byte-driver seam. */
    return 0;
#endif
}

void a20_clx_resources(a20_cluster_resource_args_t *out) {
    uint64_t *v = out->values;
    memset(v, 0, sizeof(out->values));
    v[A20_CLX_STAT_SAMPLE_MS] = timer_get_ticks() * 1000 / TICKS_PER_SEC;
    v[A20_CLX_STAT_BOOT_EPOCH] = a20_clx_boot_session();
    v[A20_CLX_STAT_MAX_ROUTES] = CLX_LIMIT_ROUTES;
    v[A20_CLX_STAT_MAX_EXPORTS] = CLX_LIMIT_SLOTS;
    v[A20_CLX_STAT_MAX_PROXIES] = CLX_LIMIT_REMOTE_EPS;
    v[A20_CLX_STAT_MAX_INFLIGHT] = CLX_LIMIT_INFLIGHT;
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    a20_clx_route_resources(v);
    a20_clx_export_resources(v);
    a20_clx_proxy_resources(v);
    a20_clx_replay_resources(v);
    spin_unlock_irqrestore(&g_clx_lock, flags);
    flags = spin_lock_irqsave(&g_clx_rx_lock);
    v[A20_CLX_STAT_RX_QUEUED] =
        (g_clx_rx_head + CLX_RX_RING - g_clx_rx_tail) % CLX_RX_RING;
    v[A20_CLX_STAT_RX_BYTES] = g_clx_rx_bytes;
    v[A20_CLX_STAT_RX_PEAK_BYTES] = g_clx_rx_peak_bytes;
    spin_unlock_irqrestore(&g_clx_rx_lock, flags);
    out->generation = 1;
}
