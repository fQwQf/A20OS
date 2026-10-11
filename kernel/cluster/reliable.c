/* Bounded end-to-end transaction replay. ACK confirms admission only;
 * replies complete calls. A session-bound slot-zero CALL acknowledges a
 * completed reply, permitting cache reuse without re-executing old calls. */
#include "cluster/clx_internal.h"
#include "mm/slab.h"

#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_SERVER
#define REPLAY_BUDGET A20_LIMIT_CLX_REASM_BYTES_ABSOLUTE
#define REPLAY_FRAME_MAX CLX_LOGICAL_MTU
#define REPLAY_SLOTS CLX_DEDUP_WINDOW
#elif CONFIG_CLUSTER_PROFILE == A20_CLX_PROFILE_MCU
#define REPLAY_BUDGET 256u
#define REPLAY_FRAME_MAX 256u
#define REPLAY_SLOTS 1u
#else
#define REPLAY_BUDGET A20_LIMIT_CLX_REASM_BYTES_DEFAULT
#define REPLAY_FRAME_MAX 4096u
#define REPLAY_SLOTS CLX_DEDUP_WINDOW
#endif

typedef struct clx_origin {
    int used;
    uint32_t hash;
    uint64_t caller_session, server_session;
    a20_clx_window_t window;
} clx_origin_t;

typedef struct clx_replay {
    int used, done, acknowledged;
    uint32_t hash, txid, slot, serve_hash;
    uint64_t caller_session, server_session, expires;
    uint8_t *frame;
    uint32_t capacity, len;
} clx_replay_t;

static clx_origin_t g_origins[CLX_LIMIT_ROUTES + 4];
static clx_replay_t g_replay[REPLAY_SLOTS];
static uint32_t g_replay_bytes, g_replay_peak_bytes;

static clx_replay_t *replay_find(uint32_t hash, uint32_t txid, uint64_t caller,
                                 uint64_t server) {
    for (uint32_t i = 0; i < REPLAY_SLOTS; i++) {
        clx_replay_t *r = &g_replay[i];
        if (r->used && r->expires > timer_get_ticks() && r->hash == hash &&
            r->txid == txid && r->caller_session == caller &&
            r->server_session == server)
            return r;
    }
    return NULL;
}

static clx_origin_t *origin_find(uint32_t hash, uint64_t caller,
                                 uint64_t server) {
    clx_origin_t *empty = NULL;
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES + 4; i++) {
        clx_origin_t *o = &g_origins[i];
        if (o->used && o->hash == hash) {
            if (o->caller_session != caller || o->server_session != server) {
                memset(&o->window, 0, sizeof(o->window));
                o->caller_session = caller;
                o->server_session = server;
            }
            return o;
        }
        if (!o->used && !empty)
            empty = o;
    }
    if (empty) {
        empty->used = 1;
        empty->hash = hash;
        empty->caller_session = caller;
        empty->server_session = server;
    }
    return empty;
}

static int reply_encode(uint8_t *frame, uint32_t capacity, uint32_t src,
                        uint32_t dst, uint32_t txid, uint64_t caller,
                        uint64_t server, const uint8_t *data, uint32_t len,
                        uint32_t error) {
    uint32_t overhead =
        A20_CLX_HDR_LEN + A20_CLX_REPLY_PREFIX + A20_CLX_CRC_LEN;
    if (capacity < overhead)
        return -1;
    if (len > capacity - overhead || len > 65536u) {
        error = A20_ERR_NO_SPACE;
        len = 0;
    }
    if (error)
        len = 0;
    /* Header-only encode first; then populate the owned frame payload. */
    a20_frame_hdr_t h = {0};
    h.ver = A20_CLX_SESSION_VER;
    h.type = A20_CLX_TYPE_CALL_REPLY;
    h.flags = A20_CLX_FLAG_RELIABLE;
    h.txid = h.seq = txid;
    h.src_hash = src;
    h.dst_hash = dst;
    h.ttl = A20_CLX_TTL_DEFAULT;
    h.csum_kind = A20_CLX_CSUM_CCITT;
    h.payload_len = A20_CLX_REPLY_PREFIX + len;
    uint8_t *p = frame + A20_CLX_HDR_LEN;
    a20_clx_session_encode(p, server, caller);
    a20_clx_put_le32(p + A20_CLX_SESSION_BYTES, error);
    if (len)
        memcpy(p + A20_CLX_REPLY_PREFIX, data, len);
    return a20_frame_encode_logical(frame, capacity, capacity, &h, p);
}

static void admission_ack(const a20_node_id_t *dst,
                          const a20_frame_hdr_t *call) {
    uint8_t frame[38], payload[4];
    a20_frame_hdr_t h = {0};
    h.ver = A20_CLX_SESSION_VER;
    h.type = A20_CLX_TYPE_ACK;
    h.txid = call->txid;
    h.src_hash = call->dst_hash;
    h.dst_hash = call->src_hash;
    h.ttl = A20_CLX_TTL_DEFAULT;
    h.csum_kind = A20_CLX_CSUM_CCITT;
    h.payload_len = 4;
    a20_clx_put_le32(payload, call->seq);
    int n = a20_frame_encode(frame, sizeof(frame), sizeof(frame), &h, payload);
    if (n > 0)
        a20_clx_core_send_frame(dst, frame, n);
}

int a20_clx_reliable_rx(const a20_node_id_t *src, a20_frame_hdr_t *h,
                        const uint8_t **payload) {
    if (!(h->flags & A20_CLX_FLAG_RELIABLE) || h->type != A20_CLX_TYPE_CALL)
        return 0;
    uint64_t local, remote;
    uint32_t caps;
    if (h->ver != A20_CLX_SESSION_VER || h->seq != h->txid ||
        a20_clx_peer_session(src, &local, &remote, &caps) < 0 ||
        !(caps & A20_CLX_CAP_RELIABLE) ||
        !a20_clx_session_matches(*payload, h->payload_len, remote, local))
        return 1;
    h->src_session = remote;
    h->dst_session = local;
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    clx_replay_t *r = replay_find(h->src_hash, h->txid, remote, local);
    if (!h->dst_slot) {
        if (h->payload_len == A20_CLX_SESSION_BYTES && r && r->done)
            r->acknowledged = 1;
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return 1;
    }
    if (r) {
        uint32_t len = r->done ? r->len : 0;
        uint8_t *copy = len ? kmalloc(len) : NULL;
        if (copy)
            memcpy(copy, r->frame, len);
        spin_unlock_irqrestore(&g_clx_lock, flags);
        admission_ack(src, h);
        if (copy) {
            a20_clx_core_send_frame(src, copy, len);
            kfree(copy);
        }
        return 1;
    }
    clx_origin_t *o = origin_find(h->src_hash, remote, local);
    uint32_t error =
        !o ? A20_ERR_NO_SPACE
        : a20_clx_window_test(&o->window, h->txid) != A20_CLX_TX_NEW
            ? A20_ERR_CANCELED
            : 0;
    uint64_t now = timer_get_ticks();
    if (!error) {
        for (uint32_t i = 0; i < REPLAY_SLOTS; i++) {
            if (!g_replay[i].used || g_replay[i].acknowledged ||
                g_replay[i].expires <= now) {
                r = &g_replay[i];
                break;
            }
        }
        if (!r)
            error = A20_ERR_NO_SPACE;
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);

    /* Reserve negotiated maximum reply storage BEFORE running a service. */
    uint32_t capacity = !error ? a20_clx_logical_mtu(src) : 0;
    if (capacity > REPLAY_FRAME_MAX)
        capacity = REPLAY_FRAME_MAX;
    uint8_t *storage = !error && capacity ? kmalloc(capacity) : NULL;
    if (!error && !storage)
        error = A20_ERR_NO_MEMORY;
    flags = spin_lock_irqsave(&g_clx_lock);
    /* RX dispatch is the only allocator of records/origins. Reply workers
     * only complete them, and slot-zero receipts only mark them reusable. */
    uint32_t old_capacity = r && r->used ? r->capacity : 0;
    if (!error && g_replay_bytes - old_capacity + capacity > REPLAY_BUDGET)
        error = A20_ERR_NO_SPACE;
    if (!error) {
        if (r->used)
            kfree(r->frame);
        memset(r, 0, sizeof(*r));
        r->used = 1;
        r->hash = h->src_hash;
        r->txid = h->txid;
        r->slot = h->dst_slot;
        r->serve_hash = h->dst_hash;
        r->caller_session = remote;
        r->server_session = local;
        r->expires = now + MS_TO_TICKS(A20_CLX_RETAIN_MS);
        r->capacity = capacity;
        r->frame = storage;
        g_replay_bytes = g_replay_bytes - old_capacity + capacity;
        if (g_replay_bytes > g_replay_peak_bytes)
            g_replay_peak_bytes = g_replay_bytes;
        a20_clx_window_commit(&o->window, h->txid);
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (error) {
        kfree(storage);
        uint8_t frame[54];
        int n = reply_encode(frame, sizeof(frame), h->dst_hash, h->src_hash,
                             h->txid, remote, local, NULL, 0, error);
        if (n > 0)
            a20_clx_core_send_frame(src, frame, n);
        return 1;
    }
    admission_ack(src, h);
    *payload += A20_CLX_SESSION_BYTES;
    h->payload_len -= A20_CLX_SESSION_BYTES;
    return 0;
}

void a20_clx_reliable_reply(const a20_node_id_t *dst, uint32_t txid,
                            uint32_t serve_hash, uint64_t caller,
                            uint64_t server, const uint8_t *data, uint32_t len,
                            uint32_t error) {
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    clx_replay_t *r = replay_find(a20_clx_node_hash(dst), txid, caller, server);
    uint8_t *copy = NULL;
    uint32_t frame_len = 0;
    if (r && !r->done && r->serve_hash == serve_hash) {
        int n = reply_encode(r->frame, r->capacity, serve_hash, r->hash, txid,
                             caller, server, data, len, error);
        if (n > 0) {
            r->len = frame_len = n;
            r->done = 1;
            copy = kmalloc(frame_len);
            if (copy)
                memcpy(copy, r->frame, frame_len);
        }
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (copy) {
        a20_clx_core_send_frame(dst, copy, frame_len);
        kfree(copy);
    }
}

void a20_clx_reliable_receipt(const a20_node_id_t *dst, uint32_t txid,
                              uint64_t local, uint64_t remote) {
    uint8_t payload[16], frame[50];
    a20_frame_hdr_t h = {0};
    h.ver = A20_CLX_SESSION_VER;
    h.type = A20_CLX_TYPE_CALL;
    h.flags = A20_CLX_FLAG_RELIABLE;
    h.txid = h.seq = txid;
    h.src_hash = a20_clx_self_hash();
    h.dst_hash = a20_clx_node_hash(dst);
    h.ttl = A20_CLX_TTL_DEFAULT;
    h.csum_kind = A20_CLX_CSUM_CCITT;
    h.payload_len = sizeof(payload);
    a20_clx_session_encode(payload, local, remote);
    int n = a20_frame_encode(frame, sizeof(frame), sizeof(frame), &h, payload);
    if (n > 0)
        a20_clx_core_send_frame(dst, frame, n);
}

void a20_clx_replay_resources(uint64_t *v) {
    for (uint32_t i = 0; i < REPLAY_SLOTS; i++)
        v[A20_CLX_STAT_REPLAY_ENTRIES] += !!g_replay[i].used;
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES + 4; i++)
        v[A20_CLX_STAT_ORIGINS] += !!g_origins[i].used;
    v[A20_CLX_STAT_REPLAY_BYTES] = g_replay_bytes;
    v[A20_CLX_STAT_REPLAY_PEAK_BYTES] = g_replay_peak_bytes;
    v[A20_CLX_STAT_MAX_REPLAY_BYTES] = REPLAY_BUDGET;
}
