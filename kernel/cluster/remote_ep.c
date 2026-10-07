/*
 * Remote endpoint proxies (docs/cluster/03-kernel-impl.md §1 integration
 * decision, §2 remote_ep.c row).
 *
 * The proxy endpoint scheme (03-§1): cluster_connect creates one ordinary
 * channel pair; the caller keeps the visible half, the cluster core keeps
 * the proxy half and its TX worker consumes the proxy's queue exactly the
 * way any local consumer would -- the channel fast path is untouched.
 *
 * Threads: proc_alloc() spawns no-argument entry points, so workers
 * register in a spawn table and pick up their payload at start.  One
 * worker per connect proxy (drains the proxy queue, sends CALL frames) and
 * one per export (drains the server's reply queue, sends CALL_REPLY).
 * Per-endpoint rather than per-transport granularity is a v0 decision
 * forced by recv blocking on a single queue; registered in 03-§2.
 *
 * Reference protocol for the proxy half (ep):
 *   create     -> the ep reference moves to the TX worker
 *   worker exit-> releases it (any exit reason, exactly once)
 *   teardown   -> takes a TEMPORARY ref for the two peer_shutdown calls
 * Teardown uses a20_channel_ep_peer_shutdown() on BOTH halves (reentrant,
 * 03-§6): shutdown(proxy) marks the caller half closed (parked caller
 * wakes); shutdown(caller half, via a20_channel_ep_peer_ref) marks the
 * proxy half closed so the worker's recv fails and it exits.
 *
 * Reference protocol for the export peer (a20_clx_export_t.peer):
 *   register   -> the export entry owns ONE reference (export.c)
 *   spawn      -> a20_clx_export_spawn_worker takes a SECOND reference as
 *                 the worker's own, bound to this registration of the entry
 *   worker exit-> releases the entry's reference ONLY when ex->peer still
 *                 is the worker's bound peer, then its own (exactly once
 *                 each); after a REPLACE reused the entry, ex->peer names
 *                 the REPLACEMENT's peer and the old worker must touch
 *                 neither it nor the new registration's pending FIFO.
 *   revoke/REPLACE -> release the entry's reference and NULL ex->peer;
 *                 the worker's own reference keeps the old endpoint alive
 *                 until its recv observes the server half going away.
 *
 * Error surfacing through the FROZEN channel API: a parked
 * channel_call/recv can only observe a message or peer_closed, so remote
 * CLOSE, link DOWN, deadline expiry and ERROR frames all surface to the
 * caller as the same peer-closed wakeup (A20_ERR_CANCELED at the syscall,
 * bit-identical to a local peer close, 03-§1).  The precise errno goes to
 * klog and the counters; registered in 01-abi/02-wire-protocol 实现期记录.
 */
#include "cluster/clx_internal.h"

#include "core/klog.h"
#include "mm/slab.h"
#include "proc/proc.h"

static a20_clx_proxy_t g_clx_proxies[CLX_LIMIT_REMOTE_EPS];
static uint32_t g_clx_proxy_count;
static uint32_t g_clx_txid_next = 1;      /* 0 = SEND only (02-§1) */

/* ---- worker spawn table (proc_alloc entries take no argument) -------------- */

enum clx_worker_kind { CLX_WK_PROXY_TX = 1, CLX_WK_EXPORT_TX };

typedef struct clx_spawn {
    int in_use;
    int kind;
    void *payload;
    void *payload2;     /* CLX_WK_EXPORT_TX: the worker's bound peer, its
                         * own reference for its whole life; NULL for
                         * CLX_WK_PROXY_TX. */
} clx_spawn_t;

static clx_spawn_t g_clx_spawn[CLX_LIMIT_REMOTE_EPS + CLX_LIMIT_SLOTS];
static spinlock_t g_clx_spawn_lock = SPINLOCK_INIT;

static void clx_proxy_tx_main(a20_clx_proxy_t *p);
static void clx_export_tx_main(a20_clx_export_t *ex,
                               a20_channel_ep_t *my_peer);

void clx_worker_entry(void)
{
    clx_spawn_t job;
    int have = 0;

    uint64_t flags = spin_lock_irqsave(&g_clx_spawn_lock);
    for (uint32_t i = 0; i < sizeof(g_clx_spawn) / sizeof(g_clx_spawn[0]); i++) {
        if (g_clx_spawn[i].in_use) {
            job = g_clx_spawn[i];
            g_clx_spawn[i].in_use = 0;
            g_clx_spawn[i].payload2 = NULL;
            have = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&g_clx_spawn_lock, flags);

    if (!have)
        return;
    if (job.kind == CLX_WK_PROXY_TX)
        clx_proxy_tx_main((a20_clx_proxy_t *)job.payload);
    else
        clx_export_tx_main((a20_clx_export_t *)job.payload,
                           (a20_channel_ep_t *)job.payload2);
    /* The thread exits; proc lifetime is the scheduler's. */
}

static int clx_spawn_queue(int kind, void *payload, void *payload2)
{
    uint64_t flags = spin_lock_irqsave(&g_clx_spawn_lock);

    for (uint32_t i = 0; i < sizeof(g_clx_spawn) / sizeof(g_clx_spawn[0]); i++) {
        if (!g_clx_spawn[i].in_use) {
            g_clx_spawn[i].in_use = 1;
            g_clx_spawn[i].kind = kind;
            g_clx_spawn[i].payload = payload;
            g_clx_spawn[i].payload2 = payload2;
            spin_unlock_irqrestore(&g_clx_spawn_lock, flags);
            if (proc_alloc(clx_worker_entry) < 0) {
                /* No thread will run: take the job back, so -1 means "not
                 * queued" and the caller can unwind the payloads without
                 * stranding them in the table (an export payload2 carries
                 * a reference). */
                flags = spin_lock_irqsave(&g_clx_spawn_lock);
                g_clx_spawn[i].in_use = 0;
                g_clx_spawn[i].payload = NULL;
                g_clx_spawn[i].payload2 = NULL;
                spin_unlock_irqrestore(&g_clx_spawn_lock, flags);
                return -1;
            }
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_clx_spawn_lock, flags);
    return -1;
}

int a20_clx_export_spawn_worker(a20_clx_export_t *ex)
{
    /* Bind the worker to THIS registration of the table entry here, at
     * spawn time: a REPLACE can retire the entry and reuse it for a new
     * registration before the worker's first instruction runs, so a
     * binding read in the worker could snap to the replacement's peer.
     * The reference moves to the worker; clx_export_tx_main releases it
     * at exit.  NULL when the registration is already gone -- the worker
     * then has nothing to do and nothing to release (revoke did). */
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    a20_channel_ep_t *peer = ex->in_use ? ex->peer : NULL;
    if (peer)
        refcount_inc(&peer->refcount);
    spin_unlock_irqrestore(&g_clx_lock, flags);

    if (clx_spawn_queue(CLX_WK_EXPORT_TX, ex, peer) < 0) {
        if (peer)
            a20_channel_ep_release(peer);
        return -1;
    }
    return 0;
}

/* ---- frame building --------------------------------------------------------- */

static uint32_t clx_route_mtu(const a20_node_id_t *dst)
{
    uint32_t transport_id, nh_len;
    uint8_t nh[A20_CLX_LINK_ADDR_MAX];

    if (a20_clx_route_lookup(dst, &transport_id, nh, &nh_len) < 0)
        return 0;
    return a20_clx_transport_mtu(transport_id);
}

static int clx_build_frame(uint8_t *buf, uint32_t cap, uint8_t type,
                           uint32_t src_hash, uint32_t dst_hash,
                           uint32_t dst_slot, uint32_t txid,
                           const uint8_t *payload, uint16_t payload_len)
{
    a20_frame_hdr_t h;

    memset(&h, 0, sizeof(h));
    h.ver = (uint8_t)A20_CLX_WIRE_VER;
    h.type = type;
    h.txid = txid;
    h.src_hash = src_hash;
    h.dst_hash = dst_hash;
    h.dst_slot = dst_slot;
    h.payload_len = payload_len;
    h.ttl = A20_CLX_TTL_DEFAULT;
    h.csum_kind = A20_CLX_CSUM_CCITT;      /* CRC always on our own wire */
    return a20_frame_encode(buf, cap, 65536, &h, payload);
}

void a20_clx_send_error(const a20_node_id_t *dst, uint32_t orig_txid,
                        uint32_t serve_hash, int errno_val)
{
    uint8_t buf[A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN];
    uint8_t payload[8];
    int n;

    (void)serve_hash;   /* used below as the wire src_hash */
    /* 02-§2 type 11 + A-08: ERROR carries 4B errno + 4B orig_txid; the
     * far side maps unknown codes to CLUSTER_UNSUPPORTED. */
    a20_clx_put_le32(payload, (uint32_t)errno_val);
    a20_clx_put_le32(payload + 4, orig_txid);
    n = clx_build_frame(buf, sizeof(buf), A20_CLX_TYPE_ERROR,
                        serve_hash, a20_clx_node_hash(dst), 0,
                        orig_txid, payload, 8);
    if (n > 0)
        a20_clx_core_send_frame(dst, buf, (uint32_t)n);
}

/* 02-§8: 本地句柄释放 -> CLOSE(dst_slot) to the remote service. */
static void clx_send_close(const a20_node_id_t *peer, uint32_t dst_slot)
{
    uint8_t buf[A20_CLX_HDR_LEN + A20_CLX_CRC_LEN];
    int n = clx_build_frame(buf, sizeof(buf), A20_CLX_TYPE_CLOSE,
                            a20_clx_self_hash(), a20_clx_node_hash(peer),
                            dst_slot, 0, NULL, 0);

    if (n > 0)
        a20_clx_core_send_frame(peer, buf, (uint32_t)n);
}

/* ---- proxy table helpers (g_clx_lock held) ---------------------------------- */

int a20_clx_proxy_inflight_add(a20_clx_proxy_t *p, uint32_t txid,
                               uint64_t deadline_tick)
{
    if (p->inflight_count >= p->inflight_cap)
        return -A20_ERR_NO_SPACE;      /* 01-abi 限额, never blocks */
    for (uint32_t i = 0; i < p->inflight_cap; i++) {
        if (p->inflight[i].deadline_tick)
            continue;
        p->inflight[i].txid = txid;
        p->inflight[i].deadline_tick = deadline_tick;
        p->inflight_count++;
        if (!p->deadline_tick || deadline_tick < p->deadline_tick)
            p->deadline_tick = deadline_tick;
        return 0;
    }
    return -A20_ERR_NO_SPACE;
}

/* Recompute the proxy's earliest-deadline cache after an in-flight entry is
 * removed (g_clx_lock held): 0 = nothing in flight.  Without this the
 * completed CALL's deadline stayed cached and deadline_scan tore the proxy
 * down 30 s after its first call even with nothing outstanding. */
static void clx_proxy_deadline_recalc(a20_clx_proxy_t *p)
{
    uint64_t earliest = 0;

    for (uint32_t i = 0; i < p->inflight_cap; i++) {
        uint64_t d = p->inflight[i].deadline_tick;
        if (d && (!earliest || d < earliest))
            earliest = d;
    }
    p->deadline_tick = earliest;
}

a20_clx_proxy_t *a20_clx_proxy_by_txid(uint32_t src_hash, uint32_t txid)
{
    for (uint32_t i = 0; i < CLX_LIMIT_REMOTE_EPS; i++) {
        a20_clx_proxy_t *p = &g_clx_proxies[i];
        if (!p->in_use || a20_clx_node_hash(&p->peer_node) != src_hash)
            continue;
        for (uint32_t j = 0; j < p->inflight_cap; j++)
            if (p->inflight[j].deadline_tick && p->inflight[j].txid == txid)
                return p;
    }
    return NULL;
}

a20_clx_proxy_t *a20_clx_proxy_by_node_slot(uint32_t src_hash, uint32_t slot)
{
    for (uint32_t i = 0; i < CLX_LIMIT_REMOTE_EPS; i++) {
        a20_clx_proxy_t *p = &g_clx_proxies[i];
        if (p->in_use && a20_clx_node_hash(&p->peer_node) == src_hash &&
            p->dst_slot == slot)
            return p;
    }
    return NULL;
}

uint32_t a20_clx_proxy_count(void)
{
    return g_clx_proxy_count;
}

/* ---- teardown ---------------------------------------------------------------- */

void a20_clx_proxy_shutdown(a20_clx_proxy_t *p, int send_close)
{
    a20_channel_ep_t *ep;
    a20_node_id_t peer;
    uint32_t slot;

    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    if (!p->in_use) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return;
    }
    peer = p->peer_node;
    slot = p->dst_slot;
    p->in_use = 0;                     /* deadline scan stops here */
    if (p->inflight) {
        kfree(p->inflight);
        p->inflight = NULL;
    }
    g_clx_proxy_count--;
    ep = p->ep;
    if (ep)
        refcount_inc(&ep->refcount);   /* temp ref for the shutdowns */
    spin_unlock_irqrestore(&g_clx_lock, flags);

    if (send_close)
        clx_send_close(&peer, slot);
    if (!ep)
        return;
    /* Both halves, 03-§1 / 02-§8: every waiter wakes with peer_closed,
     * bit-identical to a local peer close.  Reentrant (03-§6). */
    a20_channel_ep_peer_shutdown(ep);
    a20_channel_ep_t *caller = a20_channel_ep_peer_ref(ep);
    if (caller) {
        a20_channel_ep_peer_shutdown(caller);   /* wakes the TX worker */
        a20_channel_ep_release(caller);
    }
    a20_channel_ep_release(ep);                 /* drop the temp ref */
}

/* ---- RX dispatch (handlers lock internally, never sleep under the lock) ------ */

void a20_clx_proxy_rx(uint32_t transport_id, const a20_node_id_t *src,
                      const a20_frame_hdr_t *h, const uint8_t *payload)
{
    uint64_t flags;
    uint32_t src_hash;
    a20_clx_proxy_t *p;

    /* A proxy only ever consumes frames addressed to this node: replies and
     * errors to its own CALLs (dst = caller = self) and CLOSEs naming one of
     * its remote services (dst = self).  Frames addressed to another hosted
     * identity (a loopback virtual node) are not ours -- without this check
     * a reply/CLOSE addressed elsewhere could match a local proxy's
     * (src_hash, txid) or (src_hash, dst_slot) and tear it down. */
    if (h->dst_hash != a20_clx_self_hash()) {
        a20_clx_link_rx_drop(transport_id, NULL, 0);
        return;
    }

    flags = spin_lock_irqsave(&g_clx_lock);
    src_hash = a20_clx_node_hash(src);
    p = a20_clx_proxy_by_txid(src_hash, h->txid);

    if (!p && h->type == A20_CLX_TYPE_CLOSE)
        p = a20_clx_proxy_by_node_slot(src_hash, h->dst_slot);
    if (!p) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        /* Late REPLY after deadline, 02-§4: 查无事务 -> 丢弃计数. */
        a20_clx_link_rx_drop(transport_id, NULL, 0);
        return;
    }

    if (h->type == A20_CLX_TYPE_CALL_REPLY) {
        /* Complete the transaction (02-§4), then deliver the payload into
         * the caller's queue through the proxy half (03-§1): it lands in
         * the caller half's queue and wakes the parked caller. */
        for (uint32_t j = 0; j < p->inflight_cap; j++) {
            if (p->inflight[j].deadline_tick && p->inflight[j].txid == h->txid) {
                p->inflight[j].deadline_tick = 0;
                p->inflight_count--;
                break;
            }
        }
        clx_proxy_deadline_recalc(p);
        a20_channel_ep_t *ep = p->ep;
        if (ep)
            refcount_inc(&ep->refcount);
        spin_unlock_irqrestore(&g_clx_lock, flags);
        if (!ep)
            return;                    /* teardown raced us; drop */
        int64_t r = a20_channel_send(ep, payload, h->payload_len,
                                     NULL, 0, NULL, 0);
        a20_channel_ep_release(ep);
        if (r < 0)
            a20_clx_link_rx_drop(transport_id, NULL, 0);
        return;
    }

    if (h->type == A20_CLX_TYPE_ERROR) {
        /* Frozen channel surface: the parked caller cannot be handed an
         * errno, so the transaction dies with the endpoint (file header);
         * klog keeps the precise cause. */
        printf("[CLX] ERROR errno=%u txid=%u -> endpoint teardown\n",
               payload && h->payload_len >= 4 ? a20_clx_get_le32(payload) : 0u,
               h->txid);
        spin_unlock_irqrestore(&g_clx_lock, flags);
        a20_clx_proxy_shutdown(p, 0);
        return;
    }

    /* CLOSE: the remote service endpoint this proxy called is gone
     * (02-§8: v0 does not revive endpoints across re-negotiation). */
    spin_unlock_irqrestore(&g_clx_lock, flags);
    a20_clx_proxy_shutdown(p, 0);
}

void a20_clx_node_down(const a20_node_id_t *node)
{
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_clx_lock);
        a20_clx_proxy_t *victim = NULL;
        uint32_t nh = a20_clx_node_hash(node);

        for (uint32_t i = 0; i < CLX_LIMIT_REMOTE_EPS; i++) {
            a20_clx_proxy_t *p = &g_clx_proxies[i];
            if (p->in_use && a20_clx_node_hash(&p->peer_node) == nh) {
                victim = p;
                break;
            }
        }
        spin_unlock_irqrestore(&g_clx_lock, flags);
        if (!victim)
            break;
        a20_clx_proxy_shutdown(victim, 0);
    }
}

/* ---- deadline scan (housekeeping thread, 250 ms) ------------------------------ */

void a20_clx_deadline_scan(void)
{
    uint64_t now = timer_get_ticks();

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_clx_lock);
        a20_clx_proxy_t *victim = NULL;

        for (uint32_t i = 0; i < CLX_LIMIT_REMOTE_EPS; i++) {
            a20_clx_proxy_t *p = &g_clx_proxies[i];
            if (p->in_use && p->deadline_tick && p->deadline_tick <= now) {
                victim = p;
                break;
            }
        }
        spin_unlock_irqrestore(&g_clx_lock, flags);
        if (!victim)
            break;
        /* min(caller deadline, 30 s) degenerates to the link constant;
         * frozen-channel surfacing registered in 01-abi 实现期记录. */
        a20_clx_proxy_shutdown(victim, 0);
    }
}

/* ---- connect -------------------------------------------------------------------- */

static const a20_channel_type_t clx_proxy_chan_type = {
    .version = 1,
    .send_handle_types = 0,            /* 跨机 handle 拒绝, 03-§1 */
    .recv_handle_types = 0,
    .max_data_size = A20_CH_MAX_DATA,
    .max_handles = A20_CH_MAX_HANDLES,
    .flags = A20_CHAN_TYPE_REMOTE,     /* ABI maps the refusal to
                                        * A20_ERR_CLUSTER_UNSUPPORTED */
};

a20_channel_ep_t *a20_clx_proxy_create(const a20_node_id_t *node,
                                       uint32_t slot, uint32_t reliable,
                                       int *err)
{
    a20_channel_ep_t *ep0, *ep1;

    if (reliable & 0x1u /* A20_CONNECT_RELIABLE */) {
        /* v0 has no reliable.c: seq/ACK cannot be honoured, so the flag is
         * refused regardless of caps (01-abi 实现期记录). */
        *err = -A20_ERR_CLUSTER_UNSUPPORTED;
        return NULL;
    }

    ep0 = a20_channel_create(A20_CH_DEFAULT_CAP, &clx_proxy_chan_type);
    if (!ep0) {
        *err = -A20_ERR_NO_MEMORY;
        return NULL;
    }
    ep1 = a20_channel_ep_peer_ref(ep0);
    if (!ep1) {
        a20_channel_ep_release(ep0);
        *err = -A20_ERR_NO_MEMORY;
        return NULL;
    }
    /* Same initial-reference rule as a20_clx_export_register(): the pair
     * arrives with one reference per half; the proxy half is never
     * installed into a handle, so its initial reference is orphaned.
     * Drop it; the worker owns exactly one reference from here on. */
    a20_channel_ep_release(ep1);

    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    a20_clx_proxy_t *p = NULL;
    for (uint32_t i = 0; i < CLX_LIMIT_REMOTE_EPS; i++)
        if (!g_clx_proxies[i].in_use) {
            p = &g_clx_proxies[i];
            break;
        }
    if (!p) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        a20_channel_ep_release(ep1);
        a20_channel_ep_release(ep0);
        *err = -A20_ERR_NO_SPACE;      /* proxy limit (01-abi 限额) */
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->in_use = 1;
    p->ep = ep1;                       /* reference moves to the TX worker */
    p->peer_node = *node;
    p->dst_slot = slot;
    p->inflight_cap = CLX_LIMIT_INFLIGHT;
    p->inflight = kmalloc(sizeof(*p->inflight) * p->inflight_cap);
    if (!p->inflight) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        a20_channel_ep_release(ep1);
        a20_channel_ep_release(ep0);
        *err = -A20_ERR_NO_MEMORY;
        return NULL;
    }
    memset(p->inflight, 0, sizeof(*p->inflight) * p->inflight_cap);
    g_clx_proxy_count++;
    spin_unlock_irqrestore(&g_clx_lock, flags);

    if (clx_spawn_queue(CLX_WK_PROXY_TX, p, NULL) < 0) {
        /* No worker: unwind.  The worker would have released ep1. */
        uint64_t f2 = spin_lock_irqsave(&g_clx_lock);
        p->ep = NULL;
        p->in_use = 0;
        g_clx_proxy_count--;
        spin_unlock_irqrestore(&g_clx_lock, f2);
        kfree(p->inflight);
        a20_channel_ep_release(ep1);
        a20_channel_ep_release(ep0);
        *err = -A20_ERR_NO_MEMORY;
        return NULL;
    }
    return ep0;
}

/* ---- two-phase worker receive (public channel APIs only) ---------------------- */

/* recv_begin reports the head message length with ep->lock held, recv_abort
 * releases the lock without consuming; the buffer is allocated OUTSIDE the
 * lock, then the dequeue is redone.  No other consumer exists on core-held
 * endpoints, so the message is still at the head. */
static int64_t clx_worker_recv(a20_channel_ep_t *ep, uint8_t **buf_out,
                               uint32_t *len_out)
{
    uint32_t len = 0, handles = 0;
    int64_t r = a20_channel_recv_begin(ep, 0, &len, &handles);

    if (r < 0)
        return r;                       /* includes peer_closed -> exit */
    if (len == 0) {
        r = a20_channel_recv_finish(ep, NULL, &len, NULL, &handles);
        if (r >= 0) {
            *buf_out = NULL;
            *len_out = 0;
        }
        return r;
    }
    a20_channel_recv_abort(ep);

    uint8_t *buf = kmalloc(len);
    if (!buf) {
        /* Backpressure: nothing consumed, retry after a pause. */
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
        return 1;
    }
    r = a20_channel_recv_begin(ep, 0, &len, &handles);
    if (r < 0) {
        kfree(buf);
        return r;
    }
    uint32_t got = len;
    r = a20_channel_recv_finish(ep, buf, &got, NULL, &handles);
    if (r < 0) {
        kfree(buf);
        return r;
    }
    *buf_out = buf;
    *len_out = got;
    return 0;
}

static int64_t clx_worker_next_msg(a20_channel_ep_t *ep, uint8_t **buf_out,
                                   uint32_t *len_out)
{
    int64_t r = clx_worker_recv(ep, buf_out, len_out);

    if (r == 0 && *buf_out == NULL)
        return 1;                       /* zero-length message: skip */
    return r;
}

/* ---- proxy TX worker ------------------------------------------------------------ */

static void clx_proxy_tx_main(a20_clx_proxy_t *p)
{
    proc_set_name(proc_current(), "clx-txp");

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_clx_lock);
        int alive = p->in_use;
        a20_channel_ep_t *ep = p->ep;
        if (ep)
            refcount_inc(&ep->refcount);
        spin_unlock_irqrestore(&g_clx_lock, flags);
        if (!alive || !ep) {
            if (ep)
                a20_channel_ep_release(ep);
            break;
        }

        uint8_t *msg = NULL;
        uint32_t msg_len = 0;
        int64_t r = clx_worker_next_msg(ep, &msg, &msg_len);
        a20_channel_ep_release(ep);
        if (r < 0) {
            /* recv failed: the caller half was closed (handle released) or
             * teardown marked this half closed.  Only the former sends the
             * 02-§8 CLOSE(dst_slot). */
            uint64_t f2 = spin_lock_irqsave(&g_clx_lock);
            int was_live = p->in_use;
            spin_unlock_irqrestore(&g_clx_lock, f2);
            if (was_live) {
                clx_send_close(&p->peer_node, p->dst_slot);
                a20_clx_proxy_shutdown(p, 0);
            }
            break;
        }
        if (r > 0)
            continue;

        uint32_t mtu = clx_route_mtu(&p->peer_node);
        uint32_t budget = a20_frame_max_payload(mtu ? mtu : 65536);
        if (msg_len > budget) {
            /* v0 has no fragmentation (02-§5): refuse oversize messages. */
            printf("[CLX] proxy %u: %u B message over MTU budget %u\n",
                   p->id, msg_len, budget);
            kfree(msg);
            continue;
        }
        uint8_t *frame = kmalloc((uint32_t)A20_CLX_HDR_LEN + msg_len +
                                 A20_CLX_CRC_LEN);
        if (!frame) {
            kfree(msg);
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
            continue;
        }
        uint64_t f2 = spin_lock_irqsave(&g_clx_lock);
        uint32_t txid = g_clx_txid_next++;
        int n = clx_build_frame(frame, A20_CLX_HDR_LEN + (uint32_t)msg_len +
                                          A20_CLX_CRC_LEN,
                                A20_CLX_TYPE_CALL, a20_clx_self_hash(),
                                a20_clx_node_hash(&p->peer_node), p->dst_slot,
                                txid, msg, (uint16_t)msg_len);
        int ir = -1;
        if (n > 0)
            ir = a20_clx_proxy_inflight_add(
                p, txid,
                timer_get_ticks() + MS_TO_TICKS(CLX_CALL_DEADLINE_MS));
        spin_unlock_irqrestore(&g_clx_lock, f2);
        if (n > 0 && ir == 0) {
            a20_clx_core_send_frame(&p->peer_node, frame, (uint32_t)n);
        } else {
            /* In-flight cap hit: 01-abi 限额 "不得阻塞等待空位" -- drop;
             * the caller learns via the deadline teardown (registered). */
            printf("[CLX] proxy %u: inflight limit, dropping %u B\n",
                   p->id, msg_len);
        }
        kfree(frame);
        kfree(msg);
    }

    /* Worker exit: release the core's proxy-half reference (moved at
     * create).  Exactly one of the paths above set in_use=0 first. */
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    a20_channel_ep_t *ep = p->ep;
    p->ep = NULL;
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (ep)
        a20_channel_ep_release(ep);
}

/* ---- export TX worker ------------------------------------------------------------ */

static void clx_export_tx_main(a20_clx_export_t *ex, a20_channel_ep_t *my_peer)
{
    proc_set_name(proc_current(), "clx-txe");

    /* my_peer is this worker's own reference, bound at spawn time to ONE
     * registration of the table entry (a20_clx_export_spawn_worker).  It
     * doubles as the registration's IDENTITY: A20_EXPORT_REPLACE can
     * retire the entry and reuse it for a new registration while this
     * worker is still parked on the old peer's recv, after which ex->peer
     * names the REPLACEMENT's peer.  Because the reference pins my_peer,
     * the pointer value cannot be recycled while this worker lives, so a
     * plain pointer compare under g_clx_lock is a sound ownership test.
     * Everything below touches ex->* only while ex->peer == my_peer. */
    if (!my_peer)
        return;             /* registration revoked before we ever ran */

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_clx_lock);
        int alive = ex->in_use && ex->peer == my_peer;
        spin_unlock_irqrestore(&g_clx_lock, flags);
        if (!alive)
            break;                  /* revoked, or the entry was replaced */

        uint8_t *msg = NULL;
        uint32_t msg_len = 0;
        int64_t r = clx_worker_next_msg(my_peer, &msg, &msg_len);
        if (r < 0)
            break;                  /* server endpoint gone / revoked */
        if (r > 0)
            continue;

        /* FIFO reply pairing, 02-§4 实现期记录.  Ownership is re-verified
         * in the SAME lock hold as the pop: after a REPLACE the FIFO
         * belongs to the new registration, and a reply drained from the
         * old peer must never be paired against it. */
        uint64_t f2 = spin_lock_irqsave(&g_clx_lock);
        if (ex->in_use && ex->peer != my_peer) {
            spin_unlock_irqrestore(&g_clx_lock, f2);
            kfree(msg);
            break;                  /* replaced: the new worker owns ex */
        }
        a20_node_id_t caller;
        uint32_t req_txid, serve_hash;
        int have = a20_clx_export_pop_pending(ex, &caller, &req_txid,
                                              &serve_hash);
        spin_unlock_irqrestore(&g_clx_lock, f2);
        if (!have) {
            kfree(msg);
            continue;
        }

        uint8_t *frame = kmalloc((uint32_t)A20_CLX_HDR_LEN + msg_len +
                                 A20_CLX_CRC_LEN);
        if (!frame) {
            kfree(msg);
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
            continue;
        }
        /* The reply claims the SERVING node's identity (the inbound
         * CALL's dst_hash): hash(self) on a real kernel, the virtual
         * node's hash on the loopback emulation (02-§4 实现期记录). */
        f2 = spin_lock_irqsave(&g_clx_lock);
        int n = clx_build_frame(frame, A20_CLX_HDR_LEN + (uint32_t)msg_len +
                                          A20_CLX_CRC_LEN,
                                A20_CLX_TYPE_CALL_REPLY,
                                serve_hash,
                                a20_clx_node_hash(&caller), 0, req_txid,
                                msg, (uint16_t)msg_len);
        spin_unlock_irqrestore(&g_clx_lock, f2);
        if (n > 0)
            a20_clx_core_send_frame(&caller, frame, (uint32_t)n);
        kfree(frame);
        kfree(msg);
    }

    /* Exit: release the export entry's reference ONLY when the entry
     * still belongs to this worker's registration.  After a REPLACE
     * reused the entry, ex->peer names the replacement's peer -- the old
     * worker clearing/releasing it was the WA1-F5 follow-up race that
     * zombified fresh exports (selftest "REPLACE survives old
     * server-handle release").  The worker's own reference is always
     * released. */
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    a20_channel_ep_t *owned = NULL;
    if (ex->peer == my_peer) {
        owned = ex->peer;
        ex->peer = NULL;
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (owned)
        a20_channel_ep_release(owned);   /* the export's reference */
    a20_channel_ep_release(my_peer);     /* the worker's own */
}
