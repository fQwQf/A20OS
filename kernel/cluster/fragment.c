/* Single RX-owner reassembly, with bounded slots, bytes and first-fragment age.
 * g_clx_lock publishes counters; no allocation or transport I/O under it. */
#include "cluster/clx_internal.h"
#include "cluster/fragment.h"
#include "cluster/udp.h"
#include "mm/slab.h"

#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_SERVER
#define ASSEMBLY_SLOTS 64u
#define ASSEMBLY_BUDGET A20_LIMIT_CLX_REASM_BYTES_ABSOLUTE
#elif CONFIG_CLUSTER_PROFILE == A20_CLX_PROFILE_MCU
#define ASSEMBLY_SLOTS 0u
#define ASSEMBLY_BUDGET 0u
#else
#define ASSEMBLY_SLOTS 8u
#define ASSEMBLY_BUDGET A20_LIMIT_CLX_REASM_BYTES_DEFAULT
#endif

uint32_t a20_clx_logical_mtu(const a20_node_id_t *peer) {
    uint32_t id, nh_len;
    uint8_t nh[A20_CLX_LINK_ADDR_MAX];
    if (a20_clx_route_lookup(peer, &id, nh, &nh_len) < 0)
        return 0;
    uint32_t mtu = a20_clx_transport_mtu(id);
#if ASSEMBLY_SLOTS > 0 && !defined(CONFIG_MCU)
    uint64_t local, remote;
    uint32_t caps;
    if (id == A20_CLX_TRANSPORT_UDP &&
        a20_clx_peer_session(peer, &local, &remote, &caps) == 0 &&
        (caps & (A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT)) ==
            (A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT))
        return a20_clx_udp_logical_mtu(peer->bytes);
#endif
    return mtu;
}

#if ASSEMBLY_SLOTS > 0
typedef struct assembly_slot {
    int used, poisoned;
    uint64_t expires;
    uint8_t *storage;
    a20_clx_assembly_t assembly;
} assembly_slot_t;
static assembly_slot_t assemblies[ASSEMBLY_SLOTS];
static uint32_t assembly_bytes, assembly_peak;
static uint64_t completed, rejected, expired;

/* Only the RX thread calls this, including when its receive ring is empty. */
void a20_clx_fragment_expire(void) {
    uint64_t now = timer_get_ticks();
    for (uint32_t i = 0; i < ASSEMBLY_SLOTS; i++) {
        uint8_t *release = NULL;
        uint64_t f = spin_lock_irqsave(&g_clx_lock);
        assembly_slot_t *slot = &assemblies[i];
        if (slot->used && slot->expires <= now) {
            release = slot->storage;
            if (release)
                assembly_bytes -= slot->assembly.total;
            memset(slot, 0, sizeof(*slot));
            expired++;
        }
        spin_unlock_irqrestore(&g_clx_lock, f);
        kfree(release);
    }
}

/* Returns owned complete payload, or NULL for incomplete/rejected fragments. */
uint8_t *a20_clx_fragment_rx(const a20_node_id_t *src, uint32_t transport,
                             a20_frame_hdr_t *h, const uint8_t *payload) {
    uint64_t local, remote;
    uint32_t caps;
    a20_clx_fragment_part_t part;
    if (transport != A20_CLX_TRANSPORT_UDP ||
        a20_clx_peer_session(src, &local, &remote, &caps) < 0 ||
        (caps & (A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT)) !=
            (A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT) ||
        !a20_clx_session_matches(payload, h->payload_len, remote, local) ||
        a20_clx_fragment_parse(h, payload, a20_clx_transport_mtu(transport),
                               CLX_LOGICAL_MTU, &part) < 0) {
        uint64_t f = spin_lock_irqsave(&g_clx_lock);
        rejected++;
        spin_unlock_irqrestore(&g_clx_lock, f);
        return NULL;
    }
    assembly_slot_t *slot = NULL, *empty = NULL;
    uint64_t f = spin_lock_irqsave(&g_clx_lock);
    for (uint32_t i = 0; i < ASSEMBLY_SLOTS; i++) {
        assembly_slot_t *s = &assemblies[i];
        if (!s->used) {
            if (!empty)
                empty = s;
        } else if (s->assembly.header.src_hash == h->src_hash &&
                   s->assembly.header.dst_hash == h->dst_hash &&
                   s->assembly.header.type == h->type &&
                   s->assembly.header.txid == h->txid &&
                   s->assembly.source_epoch == remote &&
                   s->assembly.destination_epoch == local) {
            slot = s;
            break;
        }
    }
    if (!slot && (!empty || part.total > ASSEMBLY_BUDGET - assembly_bytes)) {
        rejected++;
        spin_unlock_irqrestore(&g_clx_lock, f);
        return NULL;
    }
    spin_unlock_irqrestore(&g_clx_lock, f);
    uint8_t *storage = !slot ? kmalloc(part.total) : NULL;
    f = spin_lock_irqsave(&g_clx_lock);
    if (!slot) {
        if (!storage) {
            rejected++;
            spin_unlock_irqrestore(&g_clx_lock, f);
            return NULL;
        }
        slot = empty;
        memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        slot->storage = storage;
        slot->expires =
            timer_get_ticks() + MS_TO_TICKS(A20_CLX_FRAGMENT_TTL_MS);
        assembly_bytes += part.total;
        if (assembly_bytes > assembly_peak)
            assembly_peak = assembly_bytes;
    }
    if (slot->poisoned) {
        rejected++;
        spin_unlock_irqrestore(&g_clx_lock, f);
        return NULL;
    }
    int result = a20_clx_fragment_feed(
        &slot->assembly, &part, slot->storage,
        slot->assembly.received ? slot->assembly.total : part.total);
    uint8_t *ready = NULL, *release = NULL;
    if (result == 1) {
        ready = slot->storage;
        *h = slot->assembly.header;
        assembly_bytes -= slot->assembly.total;
        memset(slot, 0, sizeof(*slot));
        completed++;
    } else if (result < 0) {
        release = slot->storage;
        slot->storage = NULL;
        assembly_bytes -= slot->assembly.total;
        slot->poisoned = 1; /* Keep the key until the original expiry. */
        rejected++;
    }
    spin_unlock_irqrestore(&g_clx_lock, f);
    kfree(release);
    return ready;
}
#else
void a20_clx_fragment_expire(void) {
}
uint8_t *a20_clx_fragment_rx(const a20_node_id_t *src, uint32_t transport,
                             a20_frame_hdr_t *h, const uint8_t *payload) {
    (void)src;
    (void)transport;
    (void)h;
    (void)payload;
    return NULL;
}
#endif

void a20_clx_fragment_resources(a20_cluster_resource_args_t *out) {
    memset(out->values, 0, sizeof(out->values));
    uint64_t *v = out->values;
    v[A20_CLX_REASM_SAMPLE_MS] = timer_get_ticks() * 1000 / TICKS_PER_SEC;
    v[A20_CLX_REASM_MAX_CONTEXTS] = ASSEMBLY_SLOTS;
    v[A20_CLX_REASM_MAX_BYTES] = ASSEMBLY_BUDGET;
#if ASSEMBLY_SLOTS > 0
    uint64_t f = spin_lock_irqsave(&g_clx_lock);
    for (uint32_t i = 0; i < ASSEMBLY_SLOTS; i++)
        v[A20_CLX_REASM_CONTEXTS] += !!assemblies[i].used;
    v[A20_CLX_REASM_BYTES] = assembly_bytes;
    v[A20_CLX_REASM_PEAK_BYTES] = assembly_peak;
    v[A20_CLX_REASM_COMPLETED] = completed;
    v[A20_CLX_REASM_REJECTED] = rejected;
    v[A20_CLX_REASM_EXPIRED] = expired;
    spin_unlock_irqrestore(&g_clx_lock, f);
#endif
}
