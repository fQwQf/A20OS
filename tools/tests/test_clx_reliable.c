/* Exercise production replay admission with a one-slot MCU-sized budget.
 * An early retirement must preserve running ownership, then permit reuse. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_CLUSTER_PROFILE 1
#include "../../kernel/cluster/transaction.h"
#include "../../kernel/include/uapi/a20/cluster_snapshot.h"
#define _CLX_INTERNAL_H
#define A20_CLX_PROFILE_SERVER 3
#define A20_CLX_PROFILE_MCU 1
#define CLX_LIMIT_ROUTES 4
#define CLX_DEDUP_WINDOW 64
#define TICKS_PER_SEC 1000
#define MS_TO_TICKS(ms) (ms)
#define A20_ERR_NO_SPACE 13
#define A20_ERR_CANCELED 20
#define A20_ERR_NO_MEMORY 2
#define _MM_SLAB_H

typedef struct {
    uint8_t bytes[16];
} a20_node_id_t;
typedef int spinlock_t;
static spinlock_t g_clx_lock;
static uint64_t ticks;
static unsigned executions, sends, allocations;
static int fail_alloc;
static uint8_t sent_frame[256];
static uint32_t sent_len;
static uint32_t peer_caps = A20_CLX_CAP_RELIABLE | A20_CLX_CAP_ONEWAY;
static uint64_t timer_get_ticks(void) {
    return ticks;
}
static uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!*l);
    *l = 1;
    return 0;
}
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) {
    (void)f;
    assert(*l);
    *l = 0;
}
static void *kmalloc(size_t n) {
    if (fail_alloc)
        return NULL;
    void *p = malloc(n);
    assert(p);
    allocations++;
    return p;
}
static void kfree(void *p) {
    if (p) {
        assert(allocations);
        allocations--;
        free(p);
    }
}
static uint32_t a20_clx_node_hash(const a20_node_id_t *n) {
    return a20_clx_fnv1a32(n->bytes);
}
static uint32_t a20_clx_self_hash(void) {
    a20_node_id_t n = {{0}};
    n.bytes[15] = 1;
    return a20_clx_node_hash(&n);
}
static uint32_t a20_clx_logical_mtu(const a20_node_id_t *n) {
    (void)n;
    assert(!g_clx_lock);
    return 256;
}
static int a20_clx_peer_session(const a20_node_id_t *n, uint64_t *local,
                                uint64_t *remote, uint32_t *caps) {
    (void)n;
    assert(!g_clx_lock);
    *local = 11;
    *remote = 22;
    *caps = peer_caps;
    return 0;
}
static int a20_clx_core_send_frame(const a20_node_id_t *n, const uint8_t *p,
                                   uint32_t len) {
    (void)n;
    assert(!g_clx_lock && len <= sizeof(sent_frame));
    memcpy(sent_frame, p, len);
    sent_len = len;
    sends++;
    return 0;
}
static void a20_clx_fragment_retire(uint32_t hash, uint32_t seq,
                                    uint64_t source, uint64_t destination) {
    (void)hash;
    (void)seq;
    assert(!g_clx_lock && source == 22 && destination == 11);
}
#include "../../kernel/cluster/frame.c"
#include "../../kernel/cluster/reliable.c"
static a20_node_id_t caller = {{0}};
static uint32_t receive_kind(uint32_t txid, uint32_t slot,
                             uint64_t caller_epoch, int oneway) {
    uint8_t p[17] = {0};
    a20_clx_session_encode(p, caller_epoch, 11);
    a20_frame_hdr_t h = {.ver = 1,
                         .type = oneway ? A20_CLX_TYPE_SEND : A20_CLX_TYPE_CALL,
                         .flags = A20_CLX_FLAG_RELIABLE,
                         .txid = oneway ? 0 : txid,
                         .seq = txid,
                         .src_hash = a20_clx_node_hash(&caller),
                         .dst_hash = a20_clx_self_hash(),
                         .dst_slot = slot,
                         .payload_len = slot ? 17 : 16};
    const uint8_t *body = p;
    int consumed = a20_clx_reliable_rx(&caller, &h, &body);
    if (!consumed) {
        assert(slot && h.payload_len == 1 && body == p + 16);
        assert(h.txid == txid);
        executions++;
    }
    return consumed;
}
static uint32_t receive(uint32_t txid, uint32_t slot, uint64_t caller_epoch) {
    return receive_kind(txid, slot, caller_epoch, 0);
}
static void complete(unsigned txid) {
    uint8_t data = 42;
    a20_clx_reliable_reply(&caller, txid, a20_clx_self_hash(), 22, 11, &data, 1,
                           0);
}
static unsigned last_error(void) {
    assert(sent_len >= 54 && sent_frame[3] == A20_CLX_TYPE_CALL_REPLY);
    return a20_clx_get_le32(sent_frame + 48);
}
int main(void) {
    caller.bytes[15] = 2;
    assert(!receive(1, 2, 22) && executions == 1);
    assert(receive(1, 0, 21)); /* Wrong origin epoch cannot retire anything. */
    assert(!g_replay[0].abandoned);
    assert(receive(1, 0, 22) && g_replay[0].abandoned &&
           !g_replay[0].acknowledged);
    assert(receive(2, 2, 22) && last_error() == A20_ERR_NO_SPACE &&
           executions == 1);
    assert(receive(1, 2, 22) &&
           executions == 1); /* Still running, no re-execution. */
    complete(1);
    assert(g_replay[0].done && g_replay[0].acknowledged && !last_error());
    assert(!receive(2, 2, 22) && executions == 2 && !g_replay[0].abandoned);
    assert(receive(1, 2, 22) && last_error() == A20_ERR_CANCELED &&
           executions == 2);
    assert(receive(1, 0, 22) &&
           !g_replay[0].abandoned); /* Cannot retire new owner. */
    complete(1); /* Old worker cannot complete the reused record. */
    assert(!g_replay[0].done);
    complete(2);
    assert(!g_replay[0].acknowledged);
    assert(receive(2, 0, 22) && g_replay[0].acknowledged);
    fail_alloc = 1;
    assert(receive(3, 2, 22) && last_error() == A20_ERR_NO_MEMORY &&
           executions == 2);
    fail_alloc = 0;
    assert(!receive(3, 2, 22) && executions == 3);
    complete(3);
    unsigned before = sends;
    assert(receive(3, 2, 21) && sends == before && executions == 3);
    ticks += A20_CLX_RETAIN_MS;
    assert(receive(3, 2, 22) && last_error() == A20_ERR_CANCELED);
    assert(!receive(4, 2, 22) && executions == 4);
    assert(receive(5, 0, 22)); /* Retirement overtakes the original request. */
    assert(receive(5, 2, 22) && last_error() == A20_ERR_CANCELED &&
           executions == 4);
    complete(4);
    assert(receive(4, 0, 22));
    /* SEND has a zero wire txid, a shared replay key and a small receipt
     * reservation. Lost completion cannot execute the sink twice. */
    peer_caps = A20_CLX_CAP_RELIABLE;
    before = sends;
    assert(receive_kind(6, 3, 22, 1) && executions == 4 && sends == before);
    peer_caps |= A20_CLX_CAP_ONEWAY;
    assert(receive_kind(6, 3, 21, 1) && executions == 4);
    assert(!receive_kind(6, 3, 22, 1) && executions == 5);
    assert(g_replay[0].oneway && g_replay[0].capacity == 54);
    before = sends;
    assert(receive(6, 3, 22) && sends == before); /* Mode collision. */
    a20_clx_reliable_reply(&caller, 6, a20_clx_self_hash(), 22, 11, NULL, 0, 0);
    assert(sent_len == 54 && !last_error());
    assert(receive_kind(6, 3, 22, 1) && executions == 5 && sent_len == 54);
    assert(receive(6, 0, 22) && g_replay[0].acknowledged);
    assert(!receive_kind(7, 3, 22, 1) && executions == 6);
    assert(receive_kind(6, 3, 22, 1) && executions == 6 &&
           last_error() == A20_ERR_CANCELED);
    a20_clx_reliable_reply(&caller, 7, a20_clx_self_hash(), 22, 11, NULL, 0, 0);
    peer_caps = A20_CLX_CAP_RELIABLE;
    assert(receive(71, 2, 22) && last_error() == A20_ERR_NO_SPACE);
    peer_caps |= A20_CLX_CAP_ONEWAY;
    assert(!receive(71, 2, 22) && executions == 7);
    assert(receive_kind(7, 3, 22, 1) && last_error() == A20_ERR_CANCELED);
    a20_frame_hdr_t fragment = {.type = A20_CLX_TYPE_CALL,
                                .src_hash = a20_clx_node_hash(&caller),
                                .dst_hash = a20_clx_self_hash(),
                                .dst_slot = 2,
                                .seq = 71};
    before = sends;
    assert(a20_clx_reliable_fragment_seen(&caller, &fragment, 22, 11));
    assert(sends == before + 1); /* Running: one admission ACK only. */
    complete(71);
    ticks += A20_CLX_RETRY_MS;
    before = sends;
    assert(a20_clx_reliable_fragment_seen(&caller, &fragment, 22, 11));
    assert(sends == before + 2); /* Complete: one ACK + cached reply. */
    for (unsigned i = 0; i < 64; i++)
        assert(a20_clx_reliable_fragment_seen(&caller, &fragment, 22, 11));
    assert(sends == before + 2); /* No reply amplification per fragment. */
    fragment.seq = 72;
    assert(!a20_clx_reliable_fragment_seen(&caller, &fragment, 22, 11));
    fragment.seq = 7;
    assert(a20_clx_reliable_fragment_seen(&caller, &fragment, 22, 11));
    a20_clx_reliable_receipt(&caller, 99, 11, 22);
    before = sends;
    a20_clx_reliable_retry_receipt(&caller, 11, 23);
    assert(sends == before);
    a20_clx_reliable_receipt(&caller, 88, 11, 22);
    a20_clx_reliable_retry_receipt(&caller, 11, 22);
    assert(sent_len == 50 && a20_clx_get_le32(sent_frame + 8) == 99);
    ticks += A20_CLX_RETAIN_MS;
    before = sends;
    a20_clx_reliable_retry_receipt(&caller, 11, 22);
    assert(sends == before);
    kfree(g_replay[0].frame);
    assert(!allocations);
    puts("test_clx_reliable: early/late retirement, running reservation, "
         "reuse, stale workers, OOM and no re-execution PASS");
}
