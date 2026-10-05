#include "net/socket_internal.h"
#include "net/socket_side.h"

#include "core/cpu.h"
#include "core/stdio.h"
#include "core/lock_counters.h"
#include "core/string.h"

net_bucket_t g_net_buckets[NET_SOCK_BUCKET_COUNT];
net_socket_t *g_sockets[NET_MAX_SOCKETS];
volatile int g_net_bh_pending[NET_MAX_SOCKETS];
/* Number of slots whose pending flag is currently set.  Every 0->1 and 1->0
 * flag transition goes through exchange-based helpers in socket_inet.c, so
 * the count tracks the flags exactly; sched() early-outs on it instead of
 * scanning all NET_MAX_SOCKETS slots per context switch. */
volatile int g_net_bh_pending_count;

/*
 * Debug names for /proc/a20/lock_contention and the [LOCK-STALL] report.
 *
 * These locks were never registered before this sharding, so the whole net
 * data plane was invisible to the contention audit.  Registering all of them
 * costs one lock_counters slot each, which is why LOCK_COUNTERS_MAX in
 * kernel/core/lock_counters.c is worth a second look (see
 * docs/measured/impl-notes-net.md section 8).
 */
/*
 * Debug names for /proc/a20/lock_contention and the [LOCK-STALL] report.
 *
 * These locks were never registered before this sharding, so the whole net data
 * plane was invisible to the contention audit.  Registering all of them costs
 * one lock_counters slot each, which is why LOCK_COUNTERS_MAX in
 * kernel/core/lock_counters.c is worth a second look (see
 * docs/measured/impl-notes-net.md section 8).
 *
 * The names are formatted at init rather than spelled out in a table: the table
 * would have to name every bucket of the *largest* profile, and the server
 * profile has 128 of them while the default one has 2, so a fixed list either
 * overruns the small profile or truncates the large one.
 */
#define NET_BUCKET_NAME_MAX 24
static char g_net_bucket_names[NET_SOCK_BUCKETS][NET_BUCKET_NAME_MAX];

void net_bh_slot_mark(int idx)
{
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return;
    if (__atomic_exchange_n(&g_net_bh_pending[idx], 1,
                            __ATOMIC_ACQ_REL) == 0)
        __atomic_fetch_add(&g_net_bh_pending_count, 1, __ATOMIC_ACQ_REL);
}

int net_bh_slot_clear(int idx)
{
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return 0;
    if (__atomic_exchange_n(&g_net_bh_pending[idx], 0,
                            __ATOMIC_ACQ_REL) != 0) {
        __atomic_fetch_sub(&g_net_bh_pending_count, 1, __ATOMIC_ACQ_REL);
        return 1;
    }
    return 0;
}

void net_socket_registry_init(void) {
    memset(g_sockets, 0, sizeof(g_sockets));
    memset((void *)g_net_bh_pending, 0, sizeof(g_net_bh_pending));
    g_net_bh_pending_count = 0;
    /* Every slot starts free.  The bitmap lives inside the bucket now: a
     * contiguous shard means one uint32_t word of it belongs to exactly one
     * bucket, so a register or unregister never read-modifies a word another
     * bucket lock also protects.
     *
     * The polarity is the pre-sharding one, unchanged: a CLEAR bit is a free
     * slot, a SET bit is occupied.  net_bucket_claim() picks a clear bit and
     * sets it, net_unregister_socket_locked() clears it again, and both were
     * carried over verbatim from g_sock_free ("bit n == 0 -> slot n is free").
     */
    for (int b = 0; b < NET_SOCK_BUCKET_COUNT; b++) {
        spin_init(&g_net_buckets[b].lock);
        memset(g_net_buckets[b].free_bits, 0,
               sizeof(g_net_buckets[b].free_bits));
        /* Bits past the bucket's last slot have to start *occupied*, i.e. set.
         * A bucket narrower than one bitmap word (the 8-slot embedded profile
         * has one slot per bucket) would otherwise hand out a bit offset that
         * belongs to the next bucket.  slack is in 1..31 because every profile
         * has at least one slot per bucket, so 32 - slack never hits the
         * undefined shift-by-width. */
        int slack = NET_SOCK_BUCKET_WORDS * 32 - NET_SOCK_SLOTS_PER_BUCKET;
        if (slack)
            g_net_buckets[b].free_bits[NET_SOCK_BUCKET_WORDS - 1] |=
                ~0u << (32 - slack);
    }
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        snprintf(g_net_bucket_names[b], NET_BUCKET_NAME_MAX,
                 "net_bucket_%d", b);
        spin_set_debug(&g_net_buckets[b].lock, g_net_bucket_names[b], NULL);
        lock_counters_register(&g_net_buckets[b].lock, g_net_bucket_names[b]);
    }
    spin_set_debug(&g_net_buckets[NET_SOCK_ORPHAN_BUCKET].lock,
                   "net_bucket_orphan", NULL);
}

/*
 * Claim a free slot inside one bucket, or -1 if that bucket is full.  Only this
 * bucket's own free_bits are read or written, which is the entire reason the
 * shard is a contiguous run rather than a hash (see socket_internal.h).
 */
static int net_bucket_claim(net_bucket_t *bk)
{
    for (int w = 0; w < NET_SOCK_BUCKET_WORDS; w++) {
        uint32_t free_bits = ~bk->free_bits[w];
        if (!free_bits)
            continue;
        int bit;
        for (bit = 0; bit < 32; bit++) {
            if (free_bits & (1U << bit))
                break;
        }
        bk->free_bits[w] |= (1U << bit);
        return w * 32 + bit;
    }
    return -1;
}

/*
 * Publish `s` into the table, choosing its shard here.  The caller must have
 * finished filling `s` and must not hold another socket's bucket lock: this
 * function takes shard locks itself, one at a time, and a bucket is shared by
 * NET_SOCK_SLOTS_PER_BUCKET slots, so nesting an arbitrary shard under a held
 * one is the ABBA the ascending order exists to prevent.
 *
 * The buckets are scanned as a ring starting at the registering CPU's own, so
 * concurrent registrations land on different shards first instead of all
 * queueing behind one word.  Each candidate shard is taken and released on its
 * own, so the scan never holds more than one shard lock.
 */
int net_register_socket_locked(net_socket_t *s) {
    if (!s)
        return -ENFILE;

    unsigned start = (unsigned)(cpu_current_id() % (uintptr_t)NET_SOCK_BUCKETS);
    for (int n = 0; n < NET_SOCK_BUCKETS; n++) {
        int b = (int)((start + (unsigned)n) % (unsigned)NET_SOCK_BUCKETS);
        uint64_t flags = net_bucket_lock(b);
        int k = net_bucket_claim(&g_net_buckets[b]);
        if (k >= 0) {
            int idx = (b << NET_SOCK_BUCKET_SHIFT) + k;
            g_sockets[idx] = s;
            s->in_registry = 1;
            s->reg_idx = idx;
            net_rxq_reset_locked(s);
            net_bh_slot_clear(idx);
            /* The registry's own reference, handed back by a second
             * net_socket_free() after net_unregister_socket_locked(). */
            net_socket_ref(s);
            net_bucket_unlock(b, flags);
            return 0;
        }
        net_bucket_unlock(b, flags);
    }
    return -ENFILE;
}

void net_unregister_socket_locked(net_socket_t *s) {
    if (!s || !s->in_registry)
        return;
    /*
     * Registration recorded the slot, so closing reads it back instead of
     * searching for the pointer: the search cost one comparison per slot, and
     * NET_MAX_SOCKETS is 65536 on the server profile, so it turned every
     * close() into a quarter-million cache misses.
     *
     * The identity test is what makes the recorded index usable.  A slot can
     * only be handed to a new socket after this one released it, so a
     * mismatch means the index no longer describes `s` and the slot must be
     * left to its current owner.
     */
    int i = s->reg_idx;
    int b = net_socket_bucket(s);
    /* Released while the slot is still this socket's, because that is the key
     * the packet census is indexed by. */
    net_packet_bound_release(s);
    if (i >= 0 && i < NET_MAX_SOCKETS && g_sockets[i] == s) {
        g_sockets[i] = NULL;
        net_bh_slot_clear(i);
        g_net_buckets[b].free_bits[i / 32] &= ~(1U << (i % 32));
    }
    s->in_registry = 0;
    s->reg_idx = -1;
    /* The registry's own reference is released by the caller with one more
     * net_socket_free(), *after* dropping the bucket lock: it can free the
     * socket, and obj_cache_free() is not something to run with a shard lock
     * held and interrupts disabled. */
}

/*
 * "Does this socket still own a registry slot?"  That is the right question
 * for a socket a table scan found in g_sockets[], and the wrong one for a
 * socket that never had a slot -- the accepted end of an AF_UNIX stream owns
 * none by design.  Call sites that hold a reference use net_socket_is_live()
 * in socket_internal.h instead; this predicate is kept for the questions that
 * really are about table membership.
 */
int net_socket_is_valid_locked(net_socket_t *s) {
    if (!s)
        return 0;
    return s->in_registry;
}
