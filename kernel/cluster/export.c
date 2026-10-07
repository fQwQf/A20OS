/*
 * Cluster export table (docs/cluster/03-kernel-impl.md §2 export.c row,
 * 01-abi.md §cluster_export).
 *
 * - Slot numbers are per-node monotonic and never reused; slot 0 is
 *   reserved (01-abi §2).
 * - The export owns ONE reference: the server endpoint's peer (03-§1).
 *   Inbound requests are injected by sending on the peer (they land in the
 *   server's receive queue); the export TX worker drains the peer's queue
 *   where the server's replies appear.  No reference is held on the server
 *   endpoint itself, so a server that closes its handle is detected through
 *   the peer going dead and the export is revoked (EXPORT_DROPPED event,
 *   01-abi §5).  The TX worker additionally holds its OWN reference on the
 *   peer, bound at spawn time to this registration of the entry
 *   (remote_ep.c): a REPLACE can reuse this entry while the old worker is
 *   still parked on the old peer, and the binding keeps the old worker's
 *   exit path from touching the new registration.
 * - All compile tiers share this file (03-§4); the tier difference is only
 *   the table size via CLX_LIMIT_SLOTS.
 */
#include "cluster/clx_internal.h"

static a20_clx_export_t g_clx_exports[CLX_LIMIT_SLOTS];
static uint32_t g_clx_export_count;
static uint32_t g_clx_slot_next = 1;      /* 0 reserved (01-abi §2) */
static uint64_t g_clx_dedup_drops;

/* (src_hash, txid) window, 02-§4 ">= 64 entries", overwrite-oldest. */
typedef struct clx_dedup_ent {
    uint32_t src_hash;
    uint32_t txid;
} clx_dedup_ent_t;
static clx_dedup_ent_t g_clx_dedup[CLX_DEDUP_WINDOW];
static uint32_t g_clx_dedup_idx;

int64_t a20_clx_export_register(a20_channel_ep_t *ep, const char *name,
                                uint32_t name_len, uint32_t flags,
                                uint32_t *out_slot)
{
    a20_channel_ep_t *peer;
    a20_channel_ep_t *old_peer = NULL;
    a20_clx_export_t *ex = NULL;
    a20_clx_export_t *old = NULL;
    uint32_t old_slot = 0;
    uint64_t tflags;

    if (!ep || !name || name_len == 0 || name_len > A20_CLUSTER_SERVICE_NAME_MAX)
        return -A20_ERR_INVALID_ARGUMENT;

    peer = a20_channel_ep_peer_ref(ep);   /* the one owned reference */
    if (!peer)
        /* Orphaned endpoint: no peer half to drain replies from or to
         * inject requests through, so the export could never work. */
        return -A20_ERR_INVALID_ARGUMENT;
    /* a20_channel_create() hands out the pair with one reference on each
     * half, meant to be consumed by two installed handles.  The peer half
     * is never installed anywhere here, so its initial reference is
     * orphaned: drop it and keep exactly the export's own.  Without this
     * every export would leak its peer endpoint for good. */
    a20_channel_ep_release(peer);

    tflags = spin_lock_irqsave(&g_clx_lock);
    for (uint32_t i = 0; i < CLX_LIMIT_SLOTS; i++) {
        if (g_clx_exports[i].in_use &&
            g_clx_exports[i].name_len == name_len &&
            memcmp(g_clx_exports[i].name, name, name_len) == 0) {
            old = &g_clx_exports[i];
            break;
        }
    }
    if (old && !(flags & A20_EXPORT_REPLACE)) {
        spin_unlock_irqrestore(&g_clx_lock, tflags);
        a20_channel_ep_release(peer);
        return -A20_ERR_EXISTS;       /* 01-abi 落地状态 mapping */
    }
    if (old) {
        /* A20_EXPORT_REPLACE (01-abi §2): retire the same-name export in
         * place (revoke semantics) and register the new one on a FRESH
         * slot -- slots are monotonic and never reused, so stale remote
         * callers cannot land on the replacement by accident.  Retiring
         * first guarantees a free table entry below.
         *
         * The retired export's reply worker may still be parked on the
         * OLD peer's recv; it keeps its own reference on that peer
         * (a20_clx_export_spawn_worker) and its exit path only clears
         * ex->peer while the entry still belongs to its registration, so
         * reusing this entry below cannot be clobbered by the old
         * worker's teardown (remote_ep.c clx_export_tx_main). */
        old_peer = old->peer;
        old_slot = old->slot;
        old->peer = NULL;
        old->in_use = 0;
        g_clx_export_count--;
    }
    for (uint32_t i = 0; i < CLX_LIMIT_SLOTS && !ex; i++)
        if (!g_clx_exports[i].in_use)
            ex = &g_clx_exports[i];
    if (!ex) {
        /* Only reachable without a conflict (a replaced export frees its
         * own entry): the table is genuinely full. */
        spin_unlock_irqrestore(&g_clx_lock, tflags);
        a20_channel_ep_release(peer);
        return -A20_ERR_NO_SPACE;         /* slot limit (01-abi 限额) */
    }

    memset(ex, 0, sizeof(*ex));
    ex->in_use = 1;
    ex->slot = g_clx_slot_next++;
    ex->flags = flags;
    ex->name_len = name_len;
    memcpy(ex->name, name, name_len);
    ex->name[name_len] = '\0';
    ex->peer = peer;                      /* reference moves here */
    g_clx_export_count++;
    *out_slot = ex->slot;
    spin_unlock_irqrestore(&g_clx_lock, tflags);

    if (old) {
        /* Same aftermath as a revoke: drop the replaced export's peer and
         * tell subscribers the old slot is gone (01-abi §5 EXPORT_DROPPED).
         * Its unanswered remote callers learn via their 30 s deadline. */
        if (old_peer)
            a20_channel_ep_release(old_peer);
        a20_clx_event_push(A20_CLX_EV_EXPORT_DROP, 0, old_slot, 0);
        printf("[CLX] export slot=%u replaced by slot=%u (name '%s')\n",
               old_slot, ex->slot, ex->name);
    }
    a20_clx_export_spawn_worker(ex);      /* remote_ep.c: drains replies */
    return (int64_t)ex->slot;
}

a20_clx_export_t *a20_clx_export_by_slot(uint32_t slot)
{
    for (uint32_t i = 0; i < CLX_LIMIT_SLOTS; i++)
        if (g_clx_exports[i].in_use && g_clx_exports[i].slot == slot)
            return &g_clx_exports[i];
    return NULL;
}

a20_clx_export_t *a20_clx_export_by_name(const char *name, uint32_t name_len)
{
    if (!name || name_len == 0 || name_len > A20_CLUSTER_SERVICE_NAME_MAX)
        return NULL;
    for (uint32_t i = 0; i < CLX_LIMIT_SLOTS; i++)
        if (g_clx_exports[i].in_use &&
            g_clx_exports[i].name_len == name_len &&
            memcmp(g_clx_exports[i].name, name, name_len) == 0)
            return &g_clx_exports[i];
    return NULL;
}

void a20_clx_export_revoke(a20_clx_export_t *ex, int notify)
{
    uint64_t flags;
    a20_channel_ep_t *peer;
    uint32_t slot;

    if (!ex)
        return;
    flags = spin_lock_irqsave(&g_clx_lock);
    if (!ex->in_use) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return;
    }
    peer = ex->peer;
    slot = ex->slot;
    ex->peer = NULL;
    ex->in_use = 0;
    g_clx_export_count--;
    spin_unlock_irqrestore(&g_clx_lock, flags);

    if (peer)
        a20_channel_ep_release(peer);
    if (notify) {
        a20_clx_event_push(A20_CLX_EV_EXPORT_DROP, 0, slot, 0);
        printf("[CLX] export slot=%u revoked (server endpoint gone)\n", slot);
    }
}

/* Called with g_clx_lock held: remember who called last so revoke can aim
 * its best-effort CLOSE (04-§2 实现期记录). */
void a20_clx_export_note_caller(a20_clx_export_t *ex, const a20_node_id_t *src)
{
    ex->last_caller = *src;
    ex->last_caller_valid = 1;
}

/* Called with g_clx_lock held (02-§4 dedup window). */
int a20_clx_export_dedup_seen(uint32_t src_hash, uint32_t txid)
{
    for (uint32_t i = 0; i < CLX_DEDUP_WINDOW; i++)
        if (g_clx_dedup[i].src_hash == src_hash && g_clx_dedup[i].txid == txid)
            return 1;
    g_clx_dedup[g_clx_dedup_idx].src_hash = src_hash;
    g_clx_dedup[g_clx_dedup_idx].txid = txid;
    g_clx_dedup_idx = (g_clx_dedup_idx + 1) % CLX_DEDUP_WINDOW;
    return 0;
}

uint64_t a20_clx_export_dedup_drops(void)
{
    return g_clx_dedup_drops;
}

/* Reply-pairing FIFO (g_clx_lock held), 02-§4 实现期记录. */
int a20_clx_export_push_pending(a20_clx_export_t *ex, const a20_node_id_t *src,
                                uint32_t txid, uint32_t serve_hash)
{
    if (ex->pending_count >= CLX_EXPORT_PENDING)
        return -1;
    uint32_t tail = (ex->pending_head + ex->pending_count) % CLX_EXPORT_PENDING;
    ex->pending[tail].src = *src;
    ex->pending[tail].txid = txid;
    ex->pending[tail].serve_hash = serve_hash;
    ex->pending_count++;
    return 0;
}

int a20_clx_export_pop_pending(a20_clx_export_t *ex, a20_node_id_t *src,
                               uint32_t *txid, uint32_t *serve_hash)
{
    if (!ex->pending_count)
        return 0;
    *src = ex->pending[ex->pending_head].src;
    *txid = ex->pending[ex->pending_head].txid;
    *serve_hash = ex->pending[ex->pending_head].serve_hash;
    ex->pending_head = (ex->pending_head + 1) % CLX_EXPORT_PENDING;
    ex->pending_count--;
    return 1;
}

/*
 * connect(LOCAL, name|slot) resolution (01-abi §3).
 *
 * Returns the caller half of a FRESH channel pair; the connection half is
 * injected into the exported server's receive queue as a zero-length
 * message carrying one A20_OBJ_CHANNEL_ENDPOINT handle -- the local
 * connect request.  The server picks it up like any other message
 * (handle_count > 0 distinguishes it from remote CALL injections, which
 * can never carry handles, 01-abi 跨机限制 1) and serves the connection on
 * that endpoint.
 *
 * v0 used to hand out a second reference on the export's own peer, whose
 * queue the export reply worker drains: the server's replies then raced
 * two consumers and a local caller whose reply was stolen by the reply
 * worker parked forever (the local fast path has no deadline).  The fresh
 * pair keeps cluster_connect(LOCAL) fully decoupled from the remote reply
 * path (01-abi 落地状态 #10).  Server-side convention: an exported
 * endpoint MUST accept handle-carrying messages to support LOCAL connect;
 * a server that cannot receive handles (typed channel without
 * A20_OBJ_CHANNEL_ENDPOINT in recv_handle_types) refuses at send time with
 * -A20_ERR_TYPE_MISMATCH.
 */
int a20_clx_export_connect_local(uint32_t slot, const char *name,
                                 uint32_t name_len, a20_channel_ep_t **out)
{
    a20_channel_ep_t *peer, *ep_c, *ep_conn;
    a20_clx_export_t *ex;
    uint64_t flags;
    int64_t r;

    flags = spin_lock_irqsave(&g_clx_lock);
    ex = name ? a20_clx_export_by_name(name, name_len)
              : a20_clx_export_by_slot(slot);
    if (!ex) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return -A20_ERR_NOT_FOUND;
    }
    peer = ex->peer;
    if (peer)
        refcount_inc(&peer->refcount);
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (!peer)
        return -A20_ERR_NOT_FOUND;      /* server endpoint already gone */

    /* The fresh pair: ep_c goes to the caller's new handle, ep_conn rides
     * to the server.  Same initial-reference discipline as
     * a20_clx_proxy_create(): drop ep_conn's orphaned initial reference,
     * keep exactly the one that transfers with the message. */
    ep_c = a20_channel_create(A20_CH_DEFAULT_CAP, NULL);
    if (!ep_c) {
        a20_channel_ep_release(peer);
        return -A20_ERR_NO_MEMORY;
    }
    ep_conn = a20_channel_ep_peer_ref(ep_c);
    if (!ep_conn) {
        a20_channel_ep_release(ep_c);
        a20_channel_ep_release(peer);
        return -A20_ERR_NO_MEMORY;
    }
    a20_channel_ep_release(ep_conn);    /* orphan drop; our ref remains */

    a20_ch_handle_info_t hi;
    memset(&hi, 0, sizeof(hi));
    hi.object = ep_conn;
    hi.type = A20_OBJ_CHANNEL_ENDPOINT;
    hi.transfer_rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                         A20_RIGHT_DUP | A20_RIGHT_TRANSFER;
    /* NONBLOCK: the resolution handshake must never park the caller behind
     * a full server queue (01-abi §3 "不得无限期阻塞"); a full queue fails
     * the connect instead. */
    r = a20_channel_send(peer, NULL, 0, &hi, 1, NULL, A20_MSG_NONBLOCK);
    a20_channel_ep_release(peer);
    if (r < 0) {
        a20_channel_ep_release(ep_conn);
        a20_channel_ep_release(ep_c);
        return (int)r;
    }
    a20_channel_ep_release(ep_conn);    /* ours; the message owns one now */
    *out = ep_c;
    return 0;
}

/* ---- RX: inbound CALL for one of our exports (03-§1 receive direction) -- */

void a20_clx_export_rx(uint32_t transport_id, const a20_node_id_t *src,
                       const a20_frame_hdr_t *h, const uint8_t *payload)
{
    a20_clx_export_t *ex;
    a20_channel_ep_t *peer;
    uint64_t flags;

    if (h->type == A20_CLX_TYPE_CLOSE) {
        /* A remote caller closed: its proxy sent CLOSE(dst_slot) naming
         * this side's export slot (02-§8).  Drop the unanswered request
         * pairings; late replies then fall out as unmatched. */
        flags = spin_lock_irqsave(&g_clx_lock);
        ex = a20_clx_export_by_slot(h->dst_slot);
        if (ex) {
            ex->pending_head = 0;
            ex->pending_count = 0;
        }
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return;
    }
    if (h->type != A20_CLX_TYPE_CALL)
        return;                            /* v0: SEND unused, 02-§2 note */

    flags = spin_lock_irqsave(&g_clx_lock);
    ex = a20_clx_export_by_slot(h->dst_slot);
    if (!ex || (ex->flags & A20_EXPORT_LOCAL_ONLY)) {
        uint32_t serve_hash = h->dst_hash;
        spin_unlock_irqrestore(&g_clx_lock, flags);
        /* Unknown or locally-pinned slot: 02-§8 remote non-existence maps
         * back as ERROR(NOT_FOUND); the remote caller wakes via teardown. */
        a20_clx_send_error(src, h->txid, serve_hash, A20_ERR_NOT_FOUND);
        a20_clx_link_rx_drop(transport_id, NULL, 0);
        return;
    }
    if (a20_clx_export_dedup_seen(h->src_hash, h->txid)) {
        g_clx_dedup_drops++;               /* 02-§10 dedup_drops */
        spin_unlock_irqrestore(&g_clx_lock, flags);
        a20_clx_link_rx_drop(transport_id, NULL, 0);
        return;
    }
    if (a20_clx_export_push_pending(ex, src, h->txid, h->dst_hash) < 0) {
        /* Reply FIFO full: the server cannot keep up; the request is not
         * executed, the remote caller learns via its deadline. */
        spin_unlock_irqrestore(&g_clx_lock, flags);
        a20_clx_link_rx_drop(transport_id, NULL, 0);
        return;
    }
    peer = ex->peer;
    /* Pin the peer for the injection: a concurrent revoke releases the
     * export's reference, ours keeps the endpoint alive meanwhile. */
    if (peer)
        refcount_inc(&peer->refcount);
    a20_clx_export_note_caller(ex, src);
    spin_unlock_irqrestore(&g_clx_lock, flags);

    if (!peer) {
        a20_clx_export_revoke(ex, 1);
        a20_clx_send_error(src, h->txid, h->dst_hash, A20_ERR_REMOTE_CLOSED);
        return;
    }
    /* Inject into the server's receive queue through the held peer.  The
     * request rides as an ordinary channel message (03-§1: the server sees
     * a normal endpoint; A20_CH_MAX_DATA already bounds the payload). */
    int64_t r = a20_channel_send(peer, payload, h->payload_len,
                                 NULL, 0, NULL, 0);
    a20_channel_ep_release(peer);
    if (r < 0) {
        /* The server endpoint went away mid-flight. */
        a20_clx_export_revoke(ex, 1);
        a20_clx_send_error(src, h->txid, h->dst_hash, A20_ERR_REMOTE_CLOSED);
    }
}
