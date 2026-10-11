/* Bounded routed identity sessions. Registry operations never perform I/O. */
#include "cluster/clx_internal.h"
#include "cluster/session_control.h"

typedef struct session_path {
    uint32_t transport, nh_len, mtu;
    uint8_t nh[A20_CLX_LINK_ADDR_MAX];
    uint64_t local, gateway;
} session_path_t;
typedef struct routed_session {
    int used;
    a20_node_id_t node;
    session_path_t path;
    a20_cs_session_t state;
} routed_session_t;
static routed_session_t sessions[CLX_LIMIT_ROUTES];
static uint64_t session_counter;

static uint64_t session_now(void) {
    return timer_get_ticks() * 1000 / TICKS_PER_SEC;
}
static int path_get(const a20_node_id_t *node, session_path_t *path) {
    memset(path, 0, sizeof(*path));
    if (a20_clx_route_lookup(node, &path->transport, path->nh, &path->nh_len) <
            0 ||
        path->transport == A20_CLX_TRANSPORT_LOOPBACK)
        return -1;
    const a20_clx_transport_t *t = a20_clx_transport_get(path->transport);
    uint32_t caps;
    if (!t || !t->session ||
        t->session(t->ctx, path->nh, path->nh_len, NULL, &path->local,
                   &path->gateway, &caps) < 0 ||
        !(caps & A20_CLX_CAP_RELAY) || !(caps & A20_CLX_CAP_RELIABLE))
        return -1;
    path->mtu = t->mtu;
    return 0;
}
static int same_path(const session_path_t *a, const session_path_t *b) {
    return a->transport == b->transport && a->nh_len == b->nh_len &&
           a->local == b->local && a->gateway == b->gateway &&
           !memcmp(a->nh, b->nh, a->nh_len);
}
static routed_session_t *find_locked(const a20_node_id_t *node, int create) {
    routed_session_t *free = NULL;
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        if (!sessions[i].used) {
            if (!free)
                free = &sessions[i];
        } else if (!memcmp(sessions[i].node.bytes, node->bytes, 16)) {
            return &sessions[i];
        }
    }
    if (!create || !free)
        return NULL;
    memset(free, 0, sizeof(*free));
    free->used = 1;
    free->node = *node;
    return free;
}
static void session_send(const a20_node_id_t *node, const session_path_t *path,
                         uint32_t kind, uint64_t cookie, uint64_t target_epoch,
                         uint32_t mtu) {
    a20_node_id_t self;
    uint32_t caps;
    if (a20_clx_self_get(&self, &caps) < 0)
        return;
    a20_cs_record_t r = {.kind = kind,
                         .cookie = cookie,
                         .epoch = path->local,
                         .target_epoch = target_epoch,
                         .caps = caps & ~A20_CLX_CAP_FRAGMENT,
                         .profile = CONFIG_CLUSTER_PROFILE,
                         .mtu = mtu < path->mtu ? mtu : path->mtu};
    memcpy(r.source, self.bytes, 16);
    memcpy(r.target, node->bytes, 16);
    uint8_t frame[A20_CLX_HDR_LEN + A20_CS_BYTES + A20_CLX_CRC_LEN];
    uint8_t payload[A20_CS_BYTES];
    a20_cs_encode(payload, &r);
    a20_frame_hdr_t h = {.ver = 1,
                         .type = A20_CLX_TYPE_SEND,
                         .ttl = A20_CLX_TTL_DEFAULT,
                         .src_hash = a20_clx_node_hash(&self),
                         .dst_hash = a20_clx_node_hash(node),
                         .dst_slot = A20_CS_SLOT,
                         .payload_len = A20_CS_BYTES,
                         .csum_kind = A20_CLX_CSUM_CCITT};
    int n = a20_frame_encode(frame, sizeof(frame), path->mtu, &h, payload);
    if (n > 0)
        a20_clx_core_send_frame(node, frame, n);
}

int a20_clx_routed_session(const a20_node_id_t *node, uint64_t *local,
                           uint64_t *remote, uint32_t *caps) {
    session_path_t path;
    if (path_get(node, &path) < 0)
        return -A20_ERR_NODE_UNREACHABLE;
    uint64_t now = session_now(), cookie = 0;
    int result = -A20_ERR_NODE_UNREACHABLE, cancel = 0;
    uint64_t f = spin_lock_irqsave(&g_clx_lock);
    routed_session_t *s = find_locked(node, 1);
    if (s) {
        if (!same_path(&s->path, &path)) {
            cancel = s->state.epoch != 0;
            memset(&s->state, 0, sizeof(s->state));
            s->path = path;
        }
        if (a20_cs_live(&s->state, now)) {
            *local = path.local;
            *remote = s->state.epoch;
            *caps = s->state.caps;
            result = 0;
        }
        if ((!s->state.confirmed ||
             now - s->state.confirmed_ms >= A20_CS_REFRESH_MS) &&
            !a20_cs_begin(&s->state, &session_counter, now))
            cookie = s->state.cookie;
    }
    spin_unlock_irqrestore(&g_clx_lock, f);
    if (cancel)
        a20_clx_node_down(node);
    if (cookie)
        session_send(node, &path, A20_CS_PROBE, cookie, 0, path.mtu);
    return result;
}

void a20_clx_session_rx(const a20_node_id_t *src, const a20_frame_hdr_t *h,
                        const uint8_t *payload) {
    a20_cs_record_t r;
    a20_node_id_t self;
    session_path_t path;
    if (a20_cs_decode(h, payload, &r) < 0 ||
        a20_clx_self_get(&self, NULL) < 0 || memcmp(r.source, src->bytes, 16) ||
        memcmp(r.target, self.bytes, 16) ||
        a20_clx_route_lookup(src, &path.transport, path.nh, &path.nh_len) < 0)
        return;
    const a20_clx_transport_t *t = a20_clx_transport_get(path.transport);
    uint32_t caps;
    if (!t || !t->session ||
        t->session(t->ctx, path.nh, path.nh_len, NULL, &path.local,
                   &path.gateway, &caps) < 0)
        return;
    path.mtu = t->mtu;
    /* A directly adjacent peer already has bidirectional HELLO. It can answer
     * a probe, but cannot create a second, independent epoch authority. */
    uint64_t direct_local, direct_remote;
    int direct = t->session(t->ctx, path.nh, path.nh_len, src->bytes,
                            &direct_local, &direct_remote, &caps) == 0;
    if (!direct && path_get(src, &path) < 0)
        return;
    if (r.kind == A20_CS_PROBE)
        session_send(src, &path, A20_CS_REPLY, r.cookie, r.epoch, r.mtu);
    if (direct)
        return;
    uint64_t now = session_now(), cookie = 0;
    int changed = 0;
    uint64_t f = spin_lock_irqsave(&g_clx_lock);
    routed_session_t *s = find_locked(src, r.kind == A20_CS_PROBE);
    if (s) {
        if (!same_path(&s->path, &path)) {
            changed = s->state.epoch != 0;
            memset(&s->state, 0, sizeof(s->state));
            s->path = path;
        }
        if (r.kind == A20_CS_REPLY) {
            if (r.mtu > path.mtu)
                r.mtu = path.mtu;
            int accepted = a20_cs_accept(&s->state, &r, path.local, now);
            changed |= accepted > 0;
        } else if ((!a20_cs_live(&s->state, now) ||
                    s->state.epoch != r.epoch) &&
                   !a20_cs_begin(&s->state, &session_counter, now)) {
            cookie = s->state.cookie;
        }
    }
    spin_unlock_irqrestore(&g_clx_lock, f);
    if (changed)
        a20_clx_node_down(src);
    if (cookie)
        session_send(src, &path, A20_CS_PROBE, cookie, 0, path.mtu);
}

void a20_clx_session_invalidate(const a20_node_id_t *node) {
    uint64_t f = spin_lock_irqsave(&g_clx_lock);
    routed_session_t *s = find_locked(node, 0);
    if (s)
        memset(s, 0, sizeof(*s));
    spin_unlock_irqrestore(&g_clx_lock, f);
}

uint32_t a20_clx_session_mtu(const a20_node_id_t *node) {
    uint32_t mtu = 0;
    uint64_t now = session_now();
    uint64_t f = spin_lock_irqsave(&g_clx_lock);
    routed_session_t *s = find_locked(node, 0);
    if (s && a20_cs_live(&s->state, now))
        mtu = s->state.mtu;
    spin_unlock_irqrestore(&g_clx_lock, f);
    return mtu;
}

void a20_clx_session_tick(void) {
    uint64_t now = session_now();
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        routed_session_t copy = {0};
        int send = 0, cancel = 0;
        uint64_t f = spin_lock_irqsave(&g_clx_lock);
        routed_session_t *s = &sessions[i];
        if (s->used) {
            if (s->state.confirmed && !a20_cs_live(&s->state, now)) {
                s->state.confirmed = 0;
                cancel = 1;
            }
            if (s->state.cookie && now - s->state.sent_ms >= A20_CS_RETRY_MS) {
                if (++s->state.retries >= A20_CS_RETRIES) {
                    s->state.cookie = 0;
                } else {
                    s->state.sent_ms = now;
                    send = 1;
                }
            } else if ((!s->state.confirmed ||
                        now - s->state.confirmed_ms >= A20_CS_REFRESH_MS) &&
                       !a20_cs_begin(&s->state, &session_counter, now)) {
                send = 1;
            }
            copy = *s;
        }
        spin_unlock_irqrestore(&g_clx_lock, f);
        if (!copy.used)
            continue;
        if (cancel)
            a20_clx_node_down(&copy.node);
        session_path_t current;
        if (path_get(&copy.node, &current) < 0 ||
            !same_path(&copy.path, &current)) {
            a20_clx_session_invalidate(&copy.node);
            a20_clx_node_down(&copy.node);
        } else if (send) {
            session_send(&copy.node, &copy.path, A20_CS_PROBE,
                         copy.state.cookie, 0, copy.path.mtu);
        }
    }
}
