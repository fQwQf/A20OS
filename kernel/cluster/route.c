/*
 * Cluster routing table (docs/cluster/03-kernel-impl.md §2 route.c row,
 * 01-abi.md §cluster_route).
 *
 * - v0 exact node_id match only; one best route per destination (metric
 *   kept for the table's future multi-path use; REPLACE is the recommended
 *   op and simply upserts) -- registered in 01-abi §4 实现期记录.
 * - Lookups (RX/TX hot path) never allocate; the table is a fixed array
 *   sized by the compile-time profile (03-§4), guarded by the cluster core
 *   lock.  table sizes stay small (<= 256 DEFAULT entries), so the linear
 *   scan is the honest v0 shape.
 * - A loopback route also registers the destination as a virtual node with
 *   the loopback transport (04-§2): the 4-byte next_hop is the virtual node
 *   number the loopback link layer routes on.
 */
#include "cluster/clx_internal.h"

spinlock_t g_clx_lock = SPINLOCK_INIT;

typedef struct clx_route {
    int      in_use;
    a20_node_id_t node;
    uint32_t transport_id;
    uint8_t  next_hop[16];
    uint32_t nh_len;
    uint32_t metric;
} clx_route_t;

static clx_route_t g_clx_routes[CLX_LIMIT_ROUTES];
static uint32_t g_clx_route_count;

/* ---- self identity ----------------------------------------------------- */

static a20_node_id_t g_clx_self_id;
static uint32_t g_clx_self_caps;
static int g_clx_self_set;

int a20_clx_self_is_set(void)
{
    return g_clx_self_set;
}

int a20_clx_self_get(a20_node_id_t *out_id, uint32_t *out_caps)
{
    if (!g_clx_self_set)
        return -A20_ERR_NODE_UNREACHABLE;
    if (out_id)
        *out_id = g_clx_self_id;
    if (out_caps)
        *out_caps = g_clx_self_caps;
    return 0;
}

int a20_clx_self_set(const a20_node_id_t *id, uint32_t caps)
{
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    if (g_clx_self_set) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return -A20_ERR_EXISTS;      /* 01-abi 落地状态: reuse A20_ERR_EXISTS */
    }
    g_clx_self_id = *id;
    g_clx_self_caps = caps;
    g_clx_self_set = 1;
    spin_unlock_irqrestore(&g_clx_lock, flags);
    printf("[CLX] set_self caps=0x%x hash=0x%08x\n", caps,
           a20_clx_node_hash(id));
    return 0;
}

uint32_t a20_clx_node_hash(const a20_node_id_t *id)
{
    return a20_clx_fnv1a32(id->bytes);
}

uint32_t a20_clx_self_hash(void)
{
    return a20_clx_node_hash(&g_clx_self_id);
}

/* ---- routes ------------------------------------------------------------ */

static int clx_route_find_locked(const a20_node_id_t *node)
{
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++)
        if (g_clx_routes[i].in_use &&
            memcmp(g_clx_routes[i].node.bytes, node->bytes, 16) == 0)
            return (int)i;
    return -1;
}

static int clx_route_put(const a20_node_id_t *node, uint32_t transport_id,
                         const uint8_t *next_hop, uint32_t nh_len,
                         uint32_t metric)
{
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int idx = clx_route_find_locked(node);
    int r = 0;

    if (idx >= 0) {
        /* REPLACE semantics: upsert in place (01-abi §4 default op). */
        g_clx_routes[idx].transport_id = transport_id;
        memset(g_clx_routes[idx].next_hop, 0, 16);
        memcpy(g_clx_routes[idx].next_hop, next_hop, nh_len);
        g_clx_routes[idx].nh_len = nh_len;
        g_clx_routes[idx].metric = metric;
    } else {
        if (g_clx_route_count >= CLX_LIMIT_ROUTES) {
            r = -A20_ERR_NO_SPACE;    /* 01-abi 落地状态: limit -> NO_SPACE */
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
out:
    spin_unlock_irqrestore(&g_clx_lock, flags);
    return r;
}

int a20_clx_route_add(const a20_node_id_t *node, uint32_t transport_id,
                      const uint8_t *next_hop, uint32_t nh_len, uint32_t metric)
{
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int dup = clx_route_find_locked(node) >= 0;
    spin_unlock_irqrestore(&g_clx_lock, flags);
    if (dup)
        return -A20_ERR_EXISTS;

    int r = clx_route_put(node, transport_id, next_hop, nh_len, metric);
    if (r == 0 && transport_id == A20_CLX_TRANSPORT_LOOPBACK)
        a20_clx_loopback_vnode_add(node, next_hop, nh_len);
    return r;
}

int a20_clx_route_del(const a20_node_id_t *node)
{
    uint64_t flags = spin_lock_irqsave(&g_clx_lock);
    int idx = clx_route_find_locked(node);
    if (idx < 0) {
        spin_unlock_irqrestore(&g_clx_lock, flags);
        return -A20_ERR_NOT_FOUND;
    }
    memset(&g_clx_routes[idx], 0, sizeof(g_clx_routes[idx]));
    g_clx_route_count--;
    spin_unlock_irqrestore(&g_clx_lock, flags);

    if (node)
        a20_clx_loopback_vnode_del(node);
    /* 01-abi §5 ROUTE_LOST: the destination became unreachable. */
    a20_clx_event_push(A20_CLX_EV_ROUTE_LOST, a20_clx_node_hash(node), 0, 0);
    return 0;
}

int a20_clx_route_replace(const a20_node_id_t *node, uint32_t transport_id,
                          const uint8_t *next_hop, uint32_t nh_len,
                          uint32_t metric)
{
    int r = clx_route_put(node, transport_id, next_hop, nh_len, metric);
    if (r == 0 && transport_id == A20_CLX_TRANSPORT_LOOPBACK)
        a20_clx_loopback_vnode_add(node, next_hop, nh_len);
    return r;
}

int a20_clx_route_lookup(const a20_node_id_t *node, uint32_t *out_transport,
                         uint8_t *out_nh, uint32_t *out_nh_len)
{
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

int a20_clx_node_by_hash(uint32_t hash, a20_node_id_t *out)
{
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
