/* Bounded exact-match node routes and immutable local identity.
 * Hash collisions are rejected. Configuration is serialized without holding
 * an IRQ-disabled lock over adapter startup; failed setup is rolled back.
 * Read-side lookup never allocates. See docs/cluster/03-kernel-impl.md. */
#include "cluster/clx_internal.h"
#include "cluster/udp.h"

spinlock_t g_clx_lock = SPINLOCK_INIT;

typedef struct clx_route {
    int in_use;
    a20_node_id_t node;
    uint32_t transport_id;
    uint8_t next_hop[16];
    uint32_t nh_len;
    uint32_t metric;
} clx_route_t;

static clx_route_t g_clx_routes[CLX_LIMIT_ROUTES];
static uint32_t g_clx_route_count;
static int g_clx_route_mutating;
static uint64_t g_clx_route_generation = 1;

int a20_clx_route_snapshot(a20_cluster_snapshot_args_t *out) {
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    if (out->generation && out->generation != g_clx_route_generation) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return -A20_ERR_BUSY;
    }
    out->generation = g_clx_route_generation;
    for (uint32_t i = out->index; i < CLX_LIMIT_ROUTES; i++) {
        clx_route_t *r = &g_clx_routes[i];
        if (!r->in_use)
            continue;
        out->next_index = i + 1;
        memcpy(out->node_id, r->node.bytes, 16);
        out->transport_id = r->transport_id;
        memcpy(out->next_hop, r->next_hop, 16);
        out->next_hop_len = r->nh_len;
        out->metric = r->metric;
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
    return -A20_ERR_NOT_FOUND;
}

/* ---- self identity ----------------------------------------------------- */

static a20_node_id_t g_clx_self_id;
static uint32_t g_clx_self_caps;
static int g_clx_self_set;

int a20_clx_self_is_set(void) {
    return __atomic_load_n(&g_clx_self_set, __ATOMIC_ACQUIRE);
}

int a20_clx_self_get(a20_node_id_t *out_id, uint32_t *out_caps) {
    if (!__atomic_load_n(&g_clx_self_set, __ATOMIC_ACQUIRE))
        return -A20_ERR_NODE_UNREACHABLE;
    if (out_id)
        *out_id = g_clx_self_id;
    if (out_caps)
        *out_caps = g_clx_self_caps;
    return 0;
}

int a20_clx_self_set(const a20_node_id_t *id, uint32_t caps) {
    if (!id ||
        (caps &
         ~(A20_CLX_CAP_RELAY | A20_CLX_CAP_RELIABLE | A20_CLX_CAP_LEAF)) ||
        ((caps & A20_CLX_CAP_RELAY) && (caps & A20_CLX_CAP_LEAF)))
        return -A20_ERR_INVALID_ARGUMENT;
    uint8_t any = 0, all = 0xff;
    for (uint32_t i = 0; i < 16; i++) {
        any |= id->bytes[i];
        all &= id->bytes[i];
    }
    if (!any || all == 0xff)
        return -A20_ERR_INVALID_ARGUMENT;
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    if (g_clx_self_set) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return -A20_ERR_EXISTS; /* 01-abi 落地状态: reuse A20_ERR_EXISTS */
    }
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        if (g_clx_routes[i].in_use &&
            a20_clx_node_hash(&g_clx_routes[i].node) == a20_clx_node_hash(id)) {
            spin_unlock_irqrestore(&g_clx_lock, flags);
            return -A20_ERR_EXISTS;
        }
    }
    g_clx_self_id = *id;
    g_clx_self_caps = caps | A20_CLX_CAP_RELIABLE;
    __atomic_store_n(&g_clx_self_set, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_clx_lock, flags);
    printf("[CLX] set_self caps=0x%x hash=0x%08x\n", caps,
           a20_clx_node_hash(id));
    return 0;
}

uint32_t a20_clx_node_hash(const a20_node_id_t *id) {
    return a20_clx_fnv1a32(id->bytes);
}

uint32_t a20_clx_self_hash(void) {
    return a20_clx_node_hash(&g_clx_self_id);
}

/* ---- routes ------------------------------------------------------------ */

static int clx_route_find_locked(const a20_node_id_t *node) {
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++)
        if (g_clx_routes[i].in_use &&
            memcmp(g_clx_routes[i].node.bytes, node->bytes, 16) == 0)
            return (int)i;
    return -1;
}

static int clx_route_put(const a20_node_id_t *node, uint32_t transport_id,
                         const uint8_t *next_hop, uint32_t nh_len,
                         uint32_t metric, int replace, int commit) {
    if (!node || !next_hop || nh_len > 16 || nh_len == 0 ||
        (transport_id == A20_CLX_TRANSPORT_LOOPBACK && nh_len != 4) ||
        (transport_id == A20_CLX_TRANSPORT_UDP && nh_len != 6) ||
        (transport_id == A20_CLX_TRANSPORT_UART && nh_len != 2))
        return -A20_ERR_INVALID_ARGUMENT;
    if (!a20_clx_transport_get(transport_id))
        return -A20_ERR_CLUSTER_UNSUPPORTED;
    uint8_t any = 0, all = 0xff;
    for (uint32_t i = 0; i < 16; i++) {
        any |= node->bytes[i];
        all &= node->bytes[i];
    }
    if (!any || all == 0xff)
        return -A20_ERR_INVALID_ARGUMENT;
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int idx = clx_route_find_locked(node);
    int r = 0;

    /* A v0 hash identifies one node, never an arbitrary collision winner. */
    uint32_t hash = a20_clx_node_hash(node);
    if (g_clx_self_set && a20_clx_node_hash(&g_clx_self_id) == hash) {
        r = -A20_ERR_INVALID_ARGUMENT;
        goto out;
    }
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        if (g_clx_routes[i].in_use && (int)i != idx &&
            a20_clx_node_hash(&g_clx_routes[i].node) == hash) {
            r = -A20_ERR_EXISTS;
            goto out;
        }
    }

    if ((idx >= 0 && !replace) ||
        (idx < 0 && g_clx_route_count >= CLX_LIMIT_ROUTES)) {
        r = idx >= 0 ? -A20_ERR_EXISTS : -A20_ERR_NO_SPACE;
        goto out;
    }
    if (!commit)
        goto out;
    if (idx >= 0) {
        /* REPLACE semantics: upsert in place (01-abi §4 default op). */
        g_clx_routes[idx].transport_id = transport_id;
        memset(g_clx_routes[idx].next_hop, 0, 16);
        memcpy(g_clx_routes[idx].next_hop, next_hop, nh_len);
        g_clx_routes[idx].nh_len = nh_len;
        g_clx_routes[idx].metric = metric;
    } else {
        if (g_clx_route_count >= CLX_LIMIT_ROUTES) {
            r = -A20_ERR_NO_SPACE; /* 01-abi 落地状态: limit -> NO_SPACE */
            goto out;
        }
        for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
            if (g_clx_routes[i].in_use)
                continue;
            g_clx_routes[i].in_use = 1;
            g_clx_routes[i].node = *node;
            g_clx_routes[i].transport_id = transport_id;
            memset(g_clx_routes[i].next_hop, 0, 16);
            memcpy(g_clx_routes[i].next_hop, next_hop, nh_len);
            g_clx_routes[i].nh_len = nh_len;
            g_clx_routes[i].metric = metric;
            g_clx_route_count++;
            break;
        }
    }
    g_clx_route_generation++;
out:
    spin_unlock_irqrestore(&g_clx_lock, flags);
    return r;
}

static void clx_release_unused_udp(const uint8_t *nh, uint32_t nh_len) {
#ifndef CONFIG_MCU
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int used = 0;
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        clx_route_t *r = &g_clx_routes[i];
        if (r->in_use && r->transport_id == A20_CLX_TRANSPORT_UDP &&
            r->nh_len == nh_len && memcmp(r->next_hop, nh, nh_len) == 0)
            used = 1;
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (!used)
        a20_clx_udp_peer_del(nh, nh_len);
#else
    (void)nh;
    (void)nh_len;
#endif
}

static int clx_route_update(const a20_node_id_t *node, uint32_t transport,
                            const uint8_t *nh, uint32_t nh_len, uint32_t metric,
                            int replace) {
    if (__atomic_exchange_n(&g_clx_route_mutating, 1, __ATOMIC_ACQUIRE))
        return -A20_ERR_BUSY;
    int r = clx_route_put(node, transport, nh, nh_len, metric, replace, 0);
    if (r < 0)
        goto out;
    clx_route_t old = {0};
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int idx = clx_route_find_locked(node);
    if (idx >= 0)
        old = g_clx_routes[idx];
    spin_unlock_irqrestore(&g_clx_lock, flags);
#ifndef CONFIG_MCU
    if (transport == A20_CLX_TRANSPORT_UDP) {
        r = a20_clx_udp_peer_add(node->bytes, nh, nh_len);
        if (r < 0)
            goto out;
    }
#endif
    if (transport == A20_CLX_TRANSPORT_UART) {
        r = a20_clx_uart_prepare_route(nh, nh_len);
        if (r < 0)
            goto out;
    }
    if (transport == A20_CLX_TRANSPORT_LOOPBACK) {
        r = a20_clx_loopback_vnode_add(node, nh, nh_len);
        if (r < 0)
            goto rollback;
    }
    r = clx_route_put(node, transport, nh, nh_len, metric, replace, 1);
    if (!r) {
        if (transport != A20_CLX_TRANSPORT_LOOPBACK)
            a20_clx_loopback_vnode_del(node);
        if (old.in_use &&
            (old.transport_id != transport || old.nh_len != nh_len ||
             memcmp(old.next_hop, nh, nh_len) != 0)) {
            a20_clx_session_invalidate(node);
            a20_clx_node_down(node);
        }
        if (old.in_use && old.transport_id == A20_CLX_TRANSPORT_UDP)
            clx_release_unused_udp(old.next_hop, old.nh_len);
    }
rollback:
    if (r < 0 && transport == A20_CLX_TRANSPORT_UDP)
        clx_release_unused_udp(nh, nh_len);
out:
    __atomic_store_n(&g_clx_route_mutating, 0, __ATOMIC_RELEASE);
    return r;
}

int a20_clx_route_add(const a20_node_id_t *node, uint32_t transport,
                      const uint8_t *nh, uint32_t nh_len, uint32_t metric) {
    return clx_route_update(node, transport, nh, nh_len, metric, 0);
}

int a20_clx_route_replace(const a20_node_id_t *node, uint32_t transport,
                          const uint8_t *nh, uint32_t nh_len, uint32_t metric) {
    return clx_route_update(node, transport, nh, nh_len, metric, 1);
}

int a20_clx_route_del(const a20_node_id_t *node) {
    if (!node)
        return -A20_ERR_INVALID_ARGUMENT;
    if (__atomic_exchange_n(&g_clx_route_mutating, 1, __ATOMIC_ACQUIRE))
        return -A20_ERR_BUSY;
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int idx = clx_route_find_locked(node);
    if (idx < 0) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        __atomic_store_n(&g_clx_route_mutating, 0, __ATOMIC_RELEASE);
        return -A20_ERR_NOT_FOUND;
    }
    clx_route_t old = g_clx_routes[idx];
    memset(&g_clx_routes[idx], 0, sizeof(g_clx_routes[idx]));
    g_clx_route_count--;
    g_clx_route_generation++;
    spin_unlock_irqrestore(&g_clx_lock, flags);
    a20_clx_loopback_vnode_del(node);
    if (old.transport_id == A20_CLX_TRANSPORT_UDP)
        clx_release_unused_udp(old.next_hop, old.nh_len);
    a20_clx_session_invalidate(node);
    a20_clx_node_down(node);
    a20_clx_event_push(A20_CLX_EV_ROUTE_LOST, a20_clx_node_hash(node), 0, 0);
    __atomic_store_n(&g_clx_route_mutating, 0, __ATOMIC_RELEASE);
    return 0;
}

void a20_clx_route_link_down(uint32_t transport_id, const uint8_t *nh,
                             uint32_t nh_len) {
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        a20_node_id_t node;
        uint64_t flags = spin_lock_irqsave(&g_clx_lock);
        clx_route_t *r = &g_clx_routes[i];
        int hit = r->in_use && r->transport_id == transport_id &&
                  r->nh_len == nh_len && memcmp(r->next_hop, nh, nh_len) == 0;
        if (hit)
            node = r->node;
        spin_unlock_irqrestore(&g_clx_lock, flags);
        if (hit) {
            a20_clx_session_invalidate(&node);
            a20_clx_node_down(&node);
        }
    }
}

int a20_clx_route_lookup(const a20_node_id_t *node, uint32_t *out_transport,
                         uint8_t *out_nh, uint32_t *out_nh_len) {
    a20_node_id_t self;
    uint32_t nh[1];
    uint32_t nh_len = sizeof(nh);

    /* The local node itself rides loopback (04-§2): replies from an export
     * addressed to the caller's node id must route without a table entry. */
    if (a20_clx_self_get(&self, NULL) == 0 &&
        memcmp(self.bytes, node->bytes, 16) == 0) {
        nh[0] = A20_CLX_LOOPBACK_SELF_ADDR;
        *out_transport = A20_CLX_TRANSPORT_LOOPBACK;
        memcpy(out_nh, nh, sizeof(nh));
        *out_nh_len = nh_len;
        return 0;
    }

    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int idx = clx_route_find_locked(node);
    if (idx < 0) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return -A20_ERR_NODE_UNREACHABLE;
    }
    *out_transport = g_clx_routes[idx].transport_id;
    memcpy(out_nh, g_clx_routes[idx].next_hop, 16);
    *out_nh_len = g_clx_routes[idx].nh_len;
    spin_unlock_irqrestore(&g_clx_lock, flags);
    return 0;
}

int a20_clx_node_by_hash(uint32_t hash, a20_node_id_t *out) {
    a20_node_id_t self;
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);

    if (g_clx_self_set && a20_clx_node_hash(&g_clx_self_id) == hash) {
        *out = g_clx_self_id;
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return 0;
    }
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        if (g_clx_routes[i].in_use &&
            a20_clx_node_hash(&g_clx_routes[i].node) == hash) {
            *out = g_clx_routes[i].node;
            spin_unlock_irqrestore(&g_clx_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_clx_lock, flags);
    (void)self;
    return -1;
}

void a20_clx_route_resources(uint64_t *v) {
    v[A20_CLX_STAT_ROUTES] = g_clx_route_count;
}
