/* Bounded route-rumor policy. Hints create reachability, never ALIVE state.
 * The control owner supplies a confirmed RELAY, correlates its reply and
 * validates the destination independently before publishing any service. */
#ifndef A20_CLUSTER_GOSSIP_H
#define A20_CLUSTER_GOSSIP_H
#include "cluster_membership.h"
#define A20_GOSSIP_CAP_RELAY (1u << 0)
#define A20_GOSSIP_CAP_RELIABLE (1u << 1)
#define A20_GOSSIP_ROUTE_MARK 0x80000000u
#define A20_GOSSIP_MAX_HOPS 7u
#define A20_GOSSIP_LEASE_MS 5000u

typedef struct a20_gossip_hint {
    uint8_t node[16];
    uint64_t epoch, incarnation;
    uint32_t state, caps, profile, distance;
} a20_gossip_hint_t;
typedef struct a20_gossip_route {
    uint32_t used, distance, transport, hop_len;
    uint8_t node[16], provider[16], hop[16];
    uint64_t provider_epoch, expires;
} a20_gossip_route_t;
typedef struct a20_gossip_table {
    a20_gossip_route_t *routes;
    uint32_t capacity;
    uint64_t accepted, rejected, expired;
} a20_gossip_table_t;

static inline int a20_gossip_id_valid(const uint8_t *id) {
    unsigned any = 0, all = 255;
    for (unsigned i = 0; i < 16; i++) {
        any |= id[i];
        all &= id[i];
    }
    return any && all != 255;
}
static inline int a20_gossip_id_order(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 16; i++) {
        if (a[i] < b[i])
            return -1;
        if (a[i] > b[i])
            return 1;
    }
    return 0;
}
static inline int a20_gossip_hint_valid(const a20_gossip_hint_t *h) {
    return h && a20_gossip_id_valid(h->node) && h->epoch &&
           h->state >= A20_MEMBER_ALIVE && h->state <= A20_MEMBER_DEAD &&
           h->profile >= 1 && h->profile <= 3 && !(h->caps & ~7u) &&
           h->distance < A20_GOSSIP_MAX_HOPS;
}
static inline a20_gossip_route_t *a20_gossip_find(a20_gossip_table_t *t,
                                                  const uint8_t *node) {
    for (uint32_t i = 0; i < t->capacity; i++)
        if (t->routes[i].used && a20_member_id_equal(t->routes[i].node, node))
            return &t->routes[i];
    return 0;
}
/* Only direct kernel-owned routes without the marker are explicit. Learned
 * ownership survives a daemon restart in the metric; expiration still checks
 * the current kernel route before deleting, so user replacement wins. */
static inline int a20_gossip_propose(
    a20_gossip_table_t *t, const uint8_t self[16], const uint8_t provider[16],
    uint64_t provider_epoch, uint32_t provider_caps, uint32_t transport,
    const uint8_t *hop, uint32_t hop_len, const a20_gossip_hint_t *hint,
    int explicit_route, uint64_t now, a20_gossip_route_t *proposal) {
    if (!t || !proposal || !hop || !hop_len || hop_len > 16 ||
        !a20_gossip_id_valid(self) || !a20_gossip_id_valid(provider) ||
        !provider_epoch ||
        (provider_caps & (A20_GOSSIP_CAP_RELAY | A20_GOSSIP_CAP_RELIABLE)) !=
            (A20_GOSSIP_CAP_RELAY | A20_GOSSIP_CAP_RELIABLE) ||
        !a20_gossip_hint_valid(hint) || hint->state != A20_MEMBER_ALIVE ||
        explicit_route || a20_member_id_equal(hint->node, self) ||
        a20_member_id_equal(hint->node, provider))
        return -1;
    a20_gossip_route_t *old = a20_gossip_find(t, hint->node);
    uint32_t distance = hint->distance + 1;
    if (old && old->expires > now &&
        !a20_member_id_equal(old->provider, provider) &&
        (old->distance < distance ||
         (old->distance == distance &&
          a20_gossip_id_order(old->provider, provider) < 0)))
        return -1;
    int space = old != 0;
    for (uint32_t i = 0; i < t->capacity; i++)
        space |= !t->routes[i].used;
    if (!space)
        return -1;
    *proposal = (a20_gossip_route_t){.used = 1,
                                     .distance = distance,
                                     .transport = transport,
                                     .hop_len = hop_len,
                                     .provider_epoch = provider_epoch,
                                     .expires = now + A20_GOSSIP_LEASE_MS};
    for (unsigned i = 0; i < 16; i++) {
        proposal->node[i] = hint->node[i];
        proposal->provider[i] = provider[i];
        proposal->hop[i] = i < hop_len ? hop[i] : 0;
    }
    return 0;
}
/* Commit only AFTER the kernel route mutation succeeds. Single owner. */
static inline int a20_gossip_commit(a20_gossip_table_t *t,
                                    const a20_gossip_route_t *proposal) {
    a20_gossip_route_t *slot = a20_gossip_find(t, proposal->node);
    if (!slot)
        for (uint32_t i = 0; i < t->capacity; i++)
            if (!t->routes[i].used) {
                slot = &t->routes[i];
                break;
            }
    if (!slot)
        return -1;
    *slot = *proposal;
    t->accepted++;
    return 0;
}
/* Split horizon includes already expired entries until the owner deletes
 * them; a rumor must never bounce straight back toward its provider. */
static inline int a20_gossip_advertise(a20_gossip_table_t *t,
                                       const uint8_t *node,
                                       const uint8_t *requester, uint64_t now) {
    a20_gossip_route_t *r = a20_gossip_find(t, node);
    return !a20_member_id_equal(node, requester) &&
           (!r ||
            (r->expires > now && !a20_member_id_equal(r->provider, requester)));
}
#endif
