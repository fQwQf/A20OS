/* Production routed-session registry, with transport/clock/locks substituted. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/cluster/session_control.h"
#define _CLX_INTERNAL_H
#ifndef CONFIG_CLUSTER_PROFILE
#define CONFIG_CLUSTER_PROFILE 2
#endif
#define CLX_LOGICAL_MTU (CONFIG_CLUSTER_PROFILE == 3 ? 65590u : 4096u)
#define CLX_LIMIT_ROUTES 8u
#define A20_CLX_LINK_ADDR_MAX 16u
#define A20_CLX_TRANSPORT_LOOPBACK 0u
#define A20_CLX_TRANSPORT_UDP 1u
#define A20_ERR_NODE_UNREACHABLE 1
#define TICKS_PER_SEC 1000u
#define MS_TO_TICKS(ms) (ms)
typedef struct {
    uint8_t bytes[16];
} a20_node_id_t;
typedef int spinlock_t;
static spinlock_t g_clx_lock;
static uint64_t now, gateway_epoch = 3000;
static unsigned canceled, sent;
static int reachable = 1, relay = 1, fragment = 1;
static a20_cs_record_t output;
static uint32_t remote_mtu = 256, remote_profile = 1;
static uint32_t remote_caps = A20_CLX_CAP_RELIABLE | A20_CLX_CAP_LEAF;
static uint64_t timer_get_ticks(void) {
    return now;
}
static uint64_t spin_lock_irqsave(spinlock_t *p) {
    assert(!*p);
    *p = 1;
    return 0;
}
static void spin_unlock_irqrestore(spinlock_t *p, uint64_t f) {
    (void)f;
    assert(*p);
    *p = 0;
}
static int a20_clx_route_lookup(const a20_node_id_t *n, uint32_t *id,
                                uint8_t *nh, uint32_t *len) {
    assert(!g_clx_lock);
    if (!reachable || n->bytes[0] == 255)
        return -1;
    *id = 1;
    *len = 6;
    memset(nh, 3, 6);
    return 0;
}
static int physical_session(void *ctx, const uint8_t *nh, uint32_t len,
                            const uint8_t *node, uint64_t *local,
                            uint64_t *remote, uint32_t *caps) {
    (void)ctx;
    (void)nh;
    assert(!g_clx_lock && len == 6);
    if (!reachable || (node && node[15] != 254))
        return -1;
    *local = 1000;
    *remote = gateway_epoch;
    *caps = A20_CLX_CAP_RELIABLE | (relay ? A20_CLX_CAP_RELAY : 0) |
            (fragment ? A20_CLX_CAP_FRAGMENT : 0);
    return 0;
}
typedef struct {
    uint32_t mtu;
    int (*session)(void *, const uint8_t *, uint32_t, const uint8_t *,
                   uint64_t *, uint64_t *, uint32_t *);
    void *ctx;
} a20_clx_transport_t;
static const a20_clx_transport_t *a20_clx_transport_get(uint32_t id) {
    (void)id;
    static const a20_clx_transport_t t = {1472, physical_session, NULL};
    return &t;
}
static int a20_clx_self_get(a20_node_id_t *id, uint32_t *caps) {
    assert(!g_clx_lock);
    if (id) {
        memset(id, 0, sizeof(*id));
        id->bytes[15] = 1;
    }
    if (caps)
        *caps = A20_CLX_CAP_RELIABLE;
    return 0;
}
static uint32_t a20_clx_node_hash(const a20_node_id_t *id) {
    return a20_clx_fnv1a32(id->bytes);
}
static void a20_clx_node_down(const a20_node_id_t *id) {
    (void)id;
    assert(!g_clx_lock);
    canceled++;
}
static int a20_clx_core_send_frame(const a20_node_id_t *id, const uint8_t *p,
                                   uint32_t len) {
    assert(!g_clx_lock);
    a20_frame_hdr_t h;
    const uint8_t *payload;
    assert(a20_frame_decode_session(p, len, 1472, &h, &payload) ==
           A20_FRAME_OK);
    assert(!a20_cs_decode(&h, payload, &output));
    assert(!memcmp(id->bytes, output.target, 16));
    sent++;
    return 0;
}
#include "../../kernel/cluster/frame.c"
#include "../../kernel/cluster/session.c"

static void receive(const a20_node_id_t *id, uint32_t kind, uint64_t cookie,
                    uint64_t epoch, uint64_t target_epoch) {
    a20_cs_record_t r = {.kind = kind,
                         .mtu = remote_mtu,
                         .cookie = cookie,
                         .epoch = epoch,
                         .target_epoch = target_epoch,
                         .caps = remote_caps,
                         .profile = remote_profile};
    memcpy(r.source, id->bytes, 16);
    r.target[15] = 1;
    a20_frame_hdr_t h = {.ver = 1,
                         .type = A20_CLX_TYPE_SEND,
                         .src_hash = a20_clx_fnv1a32(r.source),
                         .dst_hash = a20_clx_fnv1a32(r.target),
                         .dst_slot = A20_CS_SLOT,
                         .payload_len = A20_CS_BYTES,
                         .ttl = 7,
                         .csum_kind = A20_CLX_CSUM_CCITT};
    uint8_t payload[A20_CS_BYTES];
    a20_cs_encode(payload, &r);
    a20_clx_session_rx(id, &h, payload);
}
int main(void) {
    a20_node_id_t peer = {{0}};
    peer.bytes[15] = 2;
    uint64_t local, remote;
    uint32_t caps;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    assert(sent == 1 && output.kind == A20_CS_PROBE);
    uint64_t first = output.cookie;
    /* Wrong cookie and a previous requester's boot cannot confirm. */
    receive(&peer, A20_CS_REPLY, first + 1, 200, 1000);
    receive(&peer, A20_CS_REPLY, first, 200, 999);
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    now = 250;
    a20_clx_session_tick();
    assert(sent == 2 && output.cookie == first);
    receive(&peer, A20_CS_REPLY, first, 200, 1000);
    assert(!a20_clx_routed_session(&peer, &local, &remote, &caps));
    assert(local == 1000 && remote == 200 && a20_clx_session_mtu(&peer) == 256);
    assert(!canceled);
    /* Unsolicited probes can solicit a challenge, never roll an epoch back. */
    receive(&peer, A20_CS_PROBE, 50, 100, 0);
    uint64_t second = output.cookie;
    assert(second != first && output.kind == A20_CS_PROBE);
    assert(!a20_clx_routed_session(&peer, &local, &remote, &caps) &&
           remote == 200);
    receive(&peer, A20_CS_REPLY, first, 100, 1000);
    assert(!a20_clx_routed_session(&peer, &local, &remote, &caps) &&
           remote == 200);
    receive(&peer, A20_CS_REPLY, second, 300, 1000);
    assert(!a20_clx_routed_session(&peer, &local, &remote, &caps) &&
           remote == 300);
    assert(canceled == 1);
    receive(&peer, A20_CS_REPLY, first, 200, 1000);
    assert(!a20_clx_routed_session(&peer, &local, &remote, &caps) &&
           remote == 300);
    /* Lease expiration cancels once, retry limit retires its cookie. */
    now += A20_CS_LEASE_MS;
    a20_clx_session_tick();
    assert(canceled == 2 && !a20_clx_session_mtu(&peer));
    uint64_t retired = output.cookie;
    for (unsigned i = 0; i < A20_CS_RETRIES; i++) {
        now += A20_CS_RETRY_MS;
        a20_clx_session_tick();
    }
    receive(&peer, A20_CS_REPLY, retired, 300, 1000);
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    /* A gateway restart and route loss revoke a previously confirmed binding. */
    receive(&peer, A20_CS_REPLY, output.cookie, 300, 1000);
    assert(!a20_clx_routed_session(&peer, &local, &remote, &caps));
    gateway_epoch++;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    receive(&peer, A20_CS_REPLY, output.cookie, 300, 1000);
    reachable = 0;
    a20_clx_session_tick();
    assert(!sessions[0].used);
    reachable = 1;
    relay = 0;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    assert(!sessions[0].used);
    relay = 1;
    for (unsigned i = 0; i < CLX_LIMIT_ROUTES; i++) {
        peer.bytes[15] = i + 2;
        assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    }
    unsigned before = sent;
    peer.bytes[15] = 100;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    assert(sent == before);
    peer.bytes[15] = 2;
    a20_clx_session_invalidate(&peer);
    peer.bytes[15] = 100;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    assert(sent == before + 1);
    /* End-to-end fragmentation needs an eligible local lane and path reply.
     * A server endpoint on the far side cannot elevate the default profile. */
    memset(sessions, 0, sizeof(sessions));
    remote_mtu = A20_CS_FRAGMENT_MTU;
    remote_profile = 3;
    remote_caps = A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    assert(output.caps & A20_CLX_CAP_FRAGMENT);
    receive(&peer, A20_CS_REPLY, output.cookie, 800, 1000);
    assert(a20_clx_session_mtu(&peer) == 1472 &&
           a20_clx_session_logical_mtu(&peer) == CLX_LOGICAL_MTU);
    a20_clx_session_invalidate(&peer);
    remote_profile = 2;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    receive(&peer, A20_CS_REPLY, output.cookie, 800, 1000);
    assert(a20_clx_session_logical_mtu(&peer) == 4096);
    remote_caps &= ~A20_CLX_CAP_FRAGMENT;
    receive(&peer, A20_CS_PROBE, 90, 800, 0);
    assert(output.kind == A20_CS_REPLY &&
           !(output.caps & A20_CLX_CAP_FRAGMENT));
    fragment = 0;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    assert(!(output.caps & A20_CLX_CAP_FRAGMENT));
    remote_caps |= A20_CLX_CAP_FRAGMENT;
    receive(&peer, A20_CS_REPLY, output.cookie, 800, 1000);
    assert(a20_clx_session_logical_mtu(&peer) == 1472);
    fragment = 1;
    assert(a20_clx_routed_session(&peer, &local, &remote, &caps) < 0);
    remote_mtu = 256;
    receive(&peer, A20_CS_REPLY, output.cookie, 800, 1000);
    assert(a20_clx_session_logical_mtu(&peer) == 256);
    uint8_t path[A20_CS_BYTES] = {0};
    a20_clx_put_le32(path + 4, 1472);
    a20_clx_put_le32(path + 40, A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT);
    a20_cs_path_clamp(path, 1472, 1);
    assert(a20_clx_get_le32(path + 40) & A20_CLX_CAP_FRAGMENT);
    a20_cs_path_clamp(path, 256, 1);
    assert(a20_clx_get_le32(path + 4) == 256 &&
           !(a20_clx_get_le32(path + 40) & A20_CLX_CAP_FRAGMENT));
    a20_cs_path_clamp(path, 1472, 1);
    assert(a20_clx_get_le32(path + 4) == 256 &&
           !(a20_clx_get_le32(path + 40) & A20_CLX_CAP_FRAGMENT));
    a20_clx_put_le32(path + 4, 1472);
    a20_clx_put_le32(path + 40, A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT);
    a20_cs_path_clamp(path, 1472, 0);
    assert(!(a20_clx_get_le32(path + 40) & A20_CLX_CAP_FRAGMENT));
    /* Profile/identity/flag validation with exact-sized untrusted buffers. */
    a20_cs_record_t r = output, decoded;
    uint8_t data[A20_CS_BYTES];
    a20_cs_encode(data, &r);
    a20_frame_hdr_t h = {.ver = 1,
                         .type = A20_CLX_TYPE_SEND,
                         .src_hash = a20_clx_fnv1a32(r.source),
                         .dst_hash = a20_clx_fnv1a32(r.target),
                         .dst_slot = A20_CS_SLOT,
                         .payload_len = A20_CS_BYTES,
                         .ttl = 8,
                         .csum_kind = A20_CLX_CSUM_CCITT};
    assert(!a20_cs_decode(&h, data, &decoded));
    for (uint32_t i = 0; i < 100000; i++) {
        uint32_t n = i % (A20_CS_BYTES + 1);
        uint8_t *p = n ? malloc(n) : NULL;
        if (n)
            memcpy(p, data, n);
        h.payload_len = n;
        if (n)
            p[(i * 73u) % n] ^= (i >> 4) | 1;
        (void)a20_cs_decode(&h, p, &decoded);
        free(p);
    }
    puts("test_clx_session: correlated epochs, relay binding, MTU, leases, "
         "bounded retries/table, 100K mutations PASS");
    return 0;
}
