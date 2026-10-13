/* Execute the production fragment validator, assembler and bounded RX owner. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/cluster/fragment.h"
#include "../../kernel/include/abi/native/resource.h"
#include "../../kernel/include/uapi/a20/cluster_snapshot.h"

#define _CLX_INTERNAL_H
#ifndef CONFIG_CLUSTER_PROFILE
#define CONFIG_CLUSTER_PROFILE 2
#endif
#if CONFIG_CLUSTER_PROFILE >= 3
#define CLX_LOGICAL_MTU 65590u
#else
#define CLX_LOGICAL_MTU 4096u
#endif
#define A20_CLX_TRANSPORT_UDP 1u
#define A20_CLX_LINK_ADDR_MAX 16u
#define A20_ERR_NODE_UNREACHABLE 1
#define TICKS_PER_SEC 1000u
#define MS_TO_TICKS(n) (n)
typedef struct {
    uint8_t bytes[16];
} a20_node_id_t;
typedef int spinlock_t;
static spinlock_t g_clx_lock;
static uint64_t ticks;
static unsigned allocations, live_bytes;
static int allocation_failure;
static int request_seen, reply_pending = 1;
static uint64_t timer_get_ticks(void) {
    return ticks;
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
void *kmalloc(size_t n) {
    assert(!g_clx_lock);
    if (allocation_failure)
        return NULL;
    unsigned *p = malloc(n + sizeof(*p));
    assert(p);
    *p = n;
    allocations++;
    live_bytes += n;
    return p + 1;
}
void kfree(void *p) {
    if (!p)
        return;
    unsigned *block = (unsigned *)p - 1;
    live_bytes -= *block;
    allocations--;
    free(block);
}
static int a20_clx_route_lookup(const a20_node_id_t *p, uint32_t *id,
                                uint8_t *nh, uint32_t *len) {
    (void)p;
    (void)nh;
    *id = 1;
    *len = 0;
    return 0;
}
static uint32_t a20_clx_transport_mtu(uint32_t id) {
    (void)id;
    return 1472;
}
static int a20_clx_peer_session(const a20_node_id_t *p, uint64_t *local,
                                uint64_t *remote, uint32_t *caps) {
    (void)p;
    *local = 11;
    *remote = 22;
    *caps = A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT | A20_CLX_CAP_ONEWAY;
    return 0;
}
static uint32_t a20_clx_session_logical_mtu(const a20_node_id_t *p) {
    (void)p;
    return 0;
}
uint32_t a20_clx_udp_logical_mtu(const uint8_t node[16]) {
    (void)node;
    return CLX_LOGICAL_MTU;
}
static int a20_clx_reliable_fragment_seen(const a20_node_id_t *node,
                                          const a20_frame_hdr_t *h,
                                          uint64_t source,
                                          uint64_t destination) {
    (void)node;
    (void)h;
    assert(!g_clx_lock && source == 22 && destination == 11);
    return request_seen;
}
static int a20_clx_proxy_fragment_pending_locked(uint32_t hash, uint32_t txid,
                                                 uint64_t source,
                                                 uint64_t destination) {
    (void)hash;
    (void)txid;
    assert(g_clx_lock && source == 22 && destination == 11);
    return reply_pending;
}
#include "../../kernel/cluster/fragment.c"
#include "../../kernel/cluster/frame.c"

static uint8_t body[65556], output[65556], payload[1438];
static a20_frame_hdr_t header;

static void part(uint32_t total, unsigned index, unsigned txid) {
    const unsigned chunk = 1414;
    unsigned count = (total - 16 + chunk - 1) / chunk;
    unsigned off = index * chunk, n = total - 16 - off;
    if (n > chunk)
        n = chunk;
    header = (a20_frame_hdr_t){
        .ver = 1,
        .type = A20_CLX_TYPE_CALL,
        .flags = A20_CLX_FLAG_RELIABLE | A20_CLX_FLAG_FRAGMENTED,
        .frag = index | (index + 1 == count ? A20_CLX_FRAG_LAST : 0),
        .txid = txid,
        .seq = txid,
        .src_hash = 12,
        .dst_hash = 34,
        .dst_slot = 1,
        .payload_len = 24 + n,
        .csum_kind = A20_CLX_CSUM_CCITT};
    a20_clx_session_encode(payload, 22, 11);
    a20_clx_put_le32(payload + 16, total);
    a20_clx_put_le32(payload + 20, off);
    memcpy(payload + 24, body + 16 + off, n);
}
static int feed(a20_clx_assembly_t *a, uint32_t total, unsigned index) {
    part(total, index, 100);
    a20_clx_fragment_part_t p;
    assert(a20_clx_fragment_parse(&header, payload, 1472, 65590, &p) == 0);
    return a20_clx_fragment_feed(a, &p, output, sizeof(output));
}
int main(void) {
    uint8_t hello[32], self[16] = {1}, peer[16] = {2};
    a20_clx_build_hello_payload(peer, 0, 1, 3,
                                A20_CLX_CAP_RELIABLE | A20_CLX_CAP_FRAGMENT, 0,
                                22, hello);
    assert(!a20_clx_validate_hello(hello, sizeof(hello), 0,
                                   a20_clx_fnv1a32(peer), self, 11, 2));
    hello[A20_CLX_HELLO_OFF_PROTO_MAX] = 0;
    assert(a20_clx_validate_hello(hello, sizeof(hello), 0,
                                  a20_clx_fnv1a32(peer), self, 11, 2) < 0);
    hello[A20_CLX_HELLO_OFF_PROTO_MAX] = 1;
    hello[A20_CLX_HELLO_OFF_TIER] = 1;
    assert(a20_clx_validate_hello(hello, sizeof(hello), 0,
                                  a20_clx_fnv1a32(peer), self, 11, 2) < 0);
    hello[A20_CLX_HELLO_OFF_TIER] = 3;
    hello[A20_CLX_HELLO_OFF_CAPS] = A20_CLX_CAP_FRAGMENT;
    assert(a20_clx_validate_hello(hello, sizeof(hello), 0,
                                  a20_clx_fnv1a32(peer), self, 11, 2) < 0);
    for (unsigned i = 0; i < sizeof(body); i++)
        body[i] = (uint8_t)(i * 73);
    a20_clx_session_encode(body, 22, 11);
    /* Every legal size, tail-first ordering, then all pieces and duplicates. */
    for (unsigned total = 1439; total <= sizeof(body); total++) {
        a20_clx_assembly_t a = {0};
        unsigned count = (total - 16 + 1413) / 1414;
        assert(feed(&a, total, count - 1) == 0);
        for (unsigned i = 0; i < count - 1; i++) {
            int result = feed(&a, total, i);
            assert(result == (i + 2 == count));
            assert(feed(&a, total, i) == result);
        }
        assert(a.header.payload_len == total && a.header.frag == 0 &&
               a.header.flags == A20_CLX_FLAG_RELIABLE);
        assert(memcmp(body, output, total) == 0);
    }
    /* Private large buffers never become legal physical frames. */
    uint8_t *logical = malloc(65590);
    assert(logical);
    a20_frame_hdr_t full = {.ver = 1,
                            .type = A20_CLX_TYPE_CALL_REPLY,
                            .flags = A20_CLX_FLAG_RELIABLE,
                            .txid = 77,
                            .seq = 77,
                            .payload_len = 65556,
                            .ttl = 8,
                            .csum_kind = A20_CLX_CSUM_CCITT};
    assert(a20_frame_encode(logical, 65590, 65590, &full, body) < 0);
    assert(a20_frame_encode_logical(logical, 65590, 65590, &full, body) ==
           65590);
    const uint8_t *decoded;
    assert(a20_frame_decode_session(logical, 65590, 65590, &full, &decoded) !=
           A20_FRAME_OK);
    assert(a20_frame_decode_logical(logical, 65590, 65590, &full, &decoded) ==
           A20_FRAME_OK);
    assert(full.payload_len == 65556 && !memcmp(decoded, body, 65556));
    assert(a20_frame_encode_logical(logical, 65590, 65590, &full,
                                    logical + 32) == 65590);
    assert(a20_frame_decode_logical(logical, 65590, 65590, &full, &decoded) ==
           A20_FRAME_OK);
    assert(!memcmp(decoded, body, 65556));
    full.type = A20_CLX_TYPE_SEND;
    full.txid = 0;
    full.payload_len = 65552;
    assert(a20_frame_encode_logical(logical, 65590, 65590, &full, body) ==
           65586);
    assert(a20_frame_decode_logical(logical, 65586, 65590, &full, &decoded) ==
           A20_FRAME_OK);
    assert(full.type == A20_CLX_TYPE_SEND && full.txid == 0 &&
           full.payload_len == 65552 && !memcmp(decoded, body, 65552));
    free(logical);
    a20_clx_assembly_t a = {0};
    assert(feed(&a, 4000, 0) == 0);
    part(4000, 0, 100);
    payload[100] ^= 1;
    a20_clx_fragment_part_t p;
    assert(!a20_clx_fragment_parse(&header, payload, 1472, 4096, &p));
    assert(a20_clx_fragment_feed(&a, &p, output, sizeof(output)) == -1);
    /* Exact-sized malformed payloads, including deep metadata mutations. */
    uint32_t random = 0x20f12a55u;
    for (unsigned i = 0; i < 100000; i++) {
        part(4000, i % 3, 100);
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        switch (i % 8) {
            case 0:
                header.payload_len = random % 1439;
                break;
            case 1:
                header.frag = random;
                break;
            case 2:
                a20_clx_put_le32(payload + 16, random);
                break;
            case 3:
                a20_clx_put_le32(payload + 20, random);
                break;
            case 4:
                header.flags = random;
                break;
            case 5:
                header.seq = random;
                break;
            case 6:
                header.type = random;
                break;
            default:
                payload[random % header.payload_len] ^= 1;
                break;
        }
        uint8_t *exact = malloc(header.payload_len ? header.payload_len : 1);
        assert(exact);
        memcpy(exact, payload, header.payload_len);
        if (!a20_clx_fragment_parse(&header, exact, 1472, 4096, &p)) {
            a20_clx_assembly_t fresh = {0};
            assert(a20_clx_fragment_feed(&fresh, &p, output, sizeof(output)) >=
                   0);
        }
        free(exact);
    }
    /* Reassembly quotas, fixed first-piece deadline, poison and OOM cleanup. */
    a20_node_id_t node = {{1}};
    for (unsigned i = 1; i <= ASSEMBLY_SLOTS; i++) {
        part(4000, 0, i);
        assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    }
    assert(allocations == ASSEMBLY_SLOTS &&
           live_bytes == ASSEMBLY_SLOTS * 4000);
    part(4000, 0, ASSEMBLY_SLOTS + 1);
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    assert(allocations == ASSEMBLY_SLOTS);
    ticks = A20_CLX_FRAGMENT_TTL_MS - 1;
    part(4000, 0, 1);
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    payload[100] ^= 1;
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    assert(allocations == ASSEMBLY_SLOTS - 1 && assemblies[0].poisoned);
    ticks = A20_CLX_FRAGMENT_TTL_MS;
    a20_clx_fragment_expire();
    assert(!allocations && !live_bytes);
    allocation_failure = 1;
    part(4000, 0, 10);
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    assert(!allocations);
    allocation_failure = 0;
    /* Stale destination epochs are refused before any allocation. */
    a20_clx_put_le64(payload + 8, 10);
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    assert(!allocations);
    for (unsigned i = 0; i < 3; i++) {
        part(4000, i, 11);
        uint8_t *ready = a20_clx_fragment_rx(&node, 1, &header, payload);
        if (i < 2)
            assert(!ready);
        else {
            assert(ready && !memcmp(ready, body, 4000));
            kfree(ready);
        }
    }
    unsigned expected_expired = ASSEMBLY_SLOTS;
    /* Two SENDs both have wire txid zero. seq must separate their contexts. */
    for (unsigned index = 0; index < 3; index++) {
        for (unsigned seq = 20; seq <= 21; seq++) {
            part(4000, index, seq);
            header.type = A20_CLX_TYPE_SEND;
            header.txid = 0;
            uint8_t *ready = a20_clx_fragment_rx(&node, 1, &header, payload);
            if (index < 2)
                assert(!ready && allocations == (index == 0 ? seq - 19 : 2));
            else {
                assert(ready && header.txid == 0 && header.seq == seq &&
                       !memcmp(ready, body, 4000));
                kfree(ready);
            }
        }
    }
#if CONFIG_CLUSTER_PROFILE >= 3
    /* 64 full logical payloads exceed 4 MiB: the byte quota binds first. */
    unsigned fit = ASSEMBLY_BUDGET / sizeof(body);
    assert(fit < ASSEMBLY_SLOTS);
    for (unsigned i = 0; i < fit; i++) {
        part(sizeof(body), 0, 1000 + i);
        assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    }
    assert(allocations == fit && live_bytes == fit * sizeof(body));
    part(sizeof(body), 0, 1000 + fit);
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload));
    assert(allocations == fit);
    ticks += A20_CLX_FRAGMENT_TTL_MS;
    a20_clx_fragment_expire();
    expected_expired += fit;
#endif
    a20_cluster_resource_args_t stats;
    a20_clx_fragment_resources(&stats);
    assert(!allocations && !live_bytes && !stats.values[A20_CLX_REASM_BYTES] &&
           !stats.values[A20_CLX_REASM_CONTEXTS] &&
           stats.values[A20_CLX_REASM_COMPLETED] == 3 &&
           stats.values[A20_CLX_REASM_EXPIRED] == expected_expired);
    part(4000, 0, 100);
    request_seen = 1;
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload) && !allocations);
    request_seen = 0;
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload) &&
           allocations == 1);
    a20_clx_fragment_retire(12, 100, 21, 11);
    assert(allocations == 1); /* Wrong epoch cannot clear partial input. */
    a20_clx_fragment_retire(12, 100, 22, 11);
    assert(!allocations && !live_bytes);
    part(4000, 0, 101);
    header.type = A20_CLX_TYPE_CALL_REPLY;
    reply_pending = 0;
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload) && !allocations);
    reply_pending = 1;
    assert(!a20_clx_fragment_rx(&node, 1, &header, payload) &&
           allocations == 1);
    reply_pending = 0;
    a20_clx_fragment_expire();
    assert(!allocations &&
           !live_bytes); /* Cancelled proxy releases promptly. */
    puts("test_clx_fragment: 64118 sizes, reorder/duplicates/conflicts, slot "
         "and byte quotas, 100K mutations, expiry, session, OOM and release "
         "PASS");
}
