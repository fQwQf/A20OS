#include "net/socket_internal.h"
#include "net/socket_side.h"

#include "core/cpu.h"
#include "core/stdio.h"
#include "core/lock_counters.h"
#include "core/panic.h"
#include "core/string.h"

#include "net/socket_registry_state.h"

net_bucket_t g_net_buckets[NET_SOCK_BUCKETS];
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
 * These locks were never registered before this sharding, so the whole net data
 * plane was invisible to the contention audit.  Registering all of them costs
 * one lock_counters slot each, which is why LOCK_COUNTERS_MAX in
 * kernel/core/lock_counters.c is worth a second look.
 *
 * The names are formatted at init rather than spelled out in a table: the table
 * would have to name every bucket of the *largest* profile, and the server
 * profile has 128 of them while the default one has 2, so a fixed list either
 * overruns the small profile or truncates the large one.
 */
#define NET_BUCKET_NAME_MAX 24
static char g_net_bucket_names[NET_SOCK_BUCKETS][NET_BUCKET_NAME_MAX];

/*
 * The reference-count ledger is the Rust state machine's, together with the
 * transitions that move it: a counter is only meaningful next to the transition
 * it counts.  What is left here is the vocabulary a reader of /proc/net/status
 * or of a panic line sees, which is why the reason strings stay.
 */
static const char *const g_net_sock_ref_fault_why[NET_SOCK_REF_FAULT_REASONS] = {
    "already-freed: refs was not positive",
    "canary-mismatch: object was not a live socket",
    "reserved",
    "reserved",
};

int net_sock_ref_live(void)
{
    return a20_net_ref_live();
}

/* The Rust release has already counted the fault; this only renders it. */
static void net_sock_ref_fault(int reason, net_socket_t *s)
{
    if (reason < 0 || reason >= NET_SOCK_REF_FAULT_REASONS)
        reason = 0;
    int allocs, frees, faults;
    a20_net_ref_ledger(&allocs, &frees, &faults, NULL, 0);
    (void)s; /* only the panic line below reads it */
#if CONFIG_NET_REF_ASSERT
    panic("net_socket_free: %s s=%lx refs=%d allocs=%d frees=%d faults=%d",
          g_net_sock_ref_fault_why[reason], (unsigned long)(uintptr_t)s,
          __atomic_load_n(&s->refs, __ATOMIC_RELAXED), allocs, frees, faults);
#endif
}

int net_sock_ref_format(char *buf, size_t bufsz)
{
    int off = 0;
    int n;
    int allocs, frees, faults;
    int fault_n[NET_SOCK_REF_FAULT_REASONS];
    a20_net_ref_ledger(&allocs, &frees, &faults, fault_n, NET_SOCK_REF_FAULT_REASONS);
    n = snprintf(buf, bufsz,
                 "\nnet_sock_ref: live=%d allocs=%d frees=%d faults=%d\n",
                 a20_net_ref_live(), allocs, frees, faults);
    if (n < 0)
        return 0;
    off = n;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;
    for (int i = 0; i < NET_SOCK_REF_FAULT_REASONS; i++) {
        if (!fault_n[i])
            continue;
        n = snprintf(buf + off, bufsz - (size_t)off,
                     "net_sock_ref_fault%d: %s %d\n",
                     i, g_net_sock_ref_fault_why[i], fault_n[i]);
        if (n < 0)
            break;
        off += n;
        if ((size_t)off >= bufsz)
            return (int)bufsz - 1;
    }
    return off;
}

/*
 * Reference lifecycle.  The socket's own `refs` / `ref_magic` fields are the
 * state and the Rust module owns every transition over them: the acquire, the
 * CAS release loop, the canary check and the free decision.  What stays here is
 * the destroy sequence, because obj_cache_free()/kfree()/resource_ns_put() are
 * primitives, not state.
 */
void net_sock_ref_init(net_socket_t *s)
{
    if (!s)
        return;
    (void)a20_net_ref_init(&s->refs, &s->ref_magic);
}

/* Returns 1 when the caller must destroy the object, 0 when it must not. */
int net_sock_ref_release(net_socket_t *s)
{
    int r = a20_net_ref_release(&s->refs, &s->ref_magic);
    if (r < 0) {
        net_sock_ref_fault(-r - 1, s);
        return 0;
    }
    return r;
}

void net_bh_slot_mark(int idx)
{
    (void)a20_net_bh_mark(g_net_bh_pending, &g_net_bh_pending_count,
                          NET_MAX_SOCKETS, idx);
}

int net_bh_slot_clear(int idx)
{
    return a20_net_bh_clear(g_net_bh_pending, &g_net_bh_pending_count,
                            NET_MAX_SOCKETS, idx);
}

int net_bh_any_pending(void)
{
    return a20_net_bh_any_pending(&g_net_bh_pending_count);
}

int net_bh_slot_pending(int idx)
{
    return a20_net_bh_slot_pending(g_net_bh_pending, NET_MAX_SOCKETS, idx);
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
     * sets it, net_socket_unregister() clears it again, and both were
     * carried over verbatim from g_sock_free ("bit n == 0 -> slot n is free").
     *
     * There is no orphan shard any more.  One existed for sockets with no
     * registry slot at all -- being created, or the accepted end of an AF_UNIX
     * stream -- because that state used to be protected by the bucket lock and
     * a socket with no slot has no bucket.  Per-socket state is now protected
     * by a lock inside the socket, so the case needs nothing.
     *
     * Bits past the bucket's last slot have to start *occupied*, i.e. set.  A
     * bucket narrower than one bitmap word (the 8-slot embedded profile has one
     * slot per bucket) would otherwise hand out a bit offset that belongs to the
     * next bucket.  That rule is the Rust state machine's.
     */
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        spin_init(&g_net_buckets[b].lock);
        (void)a20_net_bucket_init(g_net_buckets[b].free_bits,
                                  NET_SOCK_BUCKET_WORDS,
                                  NET_SOCK_SLOTS_PER_BUCKET);
    }
    #if CONFIG_NET_LOCK_ASSERT
    /* Count the registry size either side of the loop below rather than
     * trusting that every register() landed.  LOCK_COUNTERS_MAX=192 was left as
     * an open budget question with the server profile's
     * 128 bucket locks eating two thirds of it, and a register() past the
     * ceiling used to be dropped without a word -- the locks work, they just
     * stop being audited, and /proc/a20/lock_contention then reports a clean run
     * for a lock nobody ever looked at.  The delta is exact here because these
     * are fresh locks: the dedup-by-pointer branch inside register() cannot
     * fire for a lock that has never been registered. */
    unsigned before = lock_counters_count();
#endif
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        snprintf(g_net_bucket_names[b], NET_BUCKET_NAME_MAX,
                 "net_bucket_%d", b);
        spin_set_debug(&g_net_buckets[b].lock, g_net_bucket_names[b], NULL);
        lock_counters_register(&g_net_buckets[b].lock, g_net_bucket_names[b]);
    }
#if CONFIG_NET_LOCK_ASSERT
    unsigned after = lock_counters_count();
    if (after - before != NET_SOCK_BUCKETS)
        net_lockcounters_short(after - before, NET_SOCK_BUCKETS,
                               lock_counters_dropped());
#endif
}

/*
 * The bitmap transactions live in the Rust state machine: claiming a free slot
 * inside one bucket (or -1 when the bucket is full) and releasing one.  Only
 * this bucket's own free_bits are read or written, which is the entire reason
 * the shard is a contiguous run rather than a hash (see socket_internal.h).
 */
static int net_bucket_claim(net_bucket_t *bk)
{
    return a20_net_bucket_claim(bk->free_bits, NET_SOCK_BUCKET_WORDS);
}

static void net_bucket_release(net_bucket_t *bk, int local)
{
    (void)a20_net_bucket_release(bk->free_bits, NET_SOCK_BUCKET_WORDS, local);
}

/*
 * Publish `s` into the table, choosing its shard here.  The caller must have
 * finished filling `s` and must hold no net lock: this function takes a shard
 * lock itself, one at a time, and then the socket's own lock to publish
 * in_registry / reg_idx, and a bucket is shared by NET_SOCK_SLOTS_PER_BUCKET
 * slots, so nesting an arbitrary shard under anything already held is exactly
 * the ABBA the order above exists to prevent.
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
            /* Shard outer, socket inner: in_registry and reg_idx belong to the
             * socket, so they are written under its lock, which is the order
             * net_bucket_scan() reads them in too. */
            uint64_t sflags = net_sock_lock(s);
            int r = a20_net_slot_publish((void **)g_sockets, NET_MAX_SOCKETS, idx,
                                         s, &s->in_registry, &s->reg_idx,
                                         &s->refs, g_net_bh_pending,
                                         &g_net_bh_pending_count);
            net_rxq_reset_locked(s);
            net_sock_unlock(s, sflags);
            if (r != 0) {
                /* The claim and the table disagreed.  Put the bit back rather
                 * than leaving a slot that no socket can ever release. */
                net_bucket_release(&g_net_buckets[b], k);
                net_bucket_unlock(b, flags);
                return -ENFILE;
            }
            net_bucket_unlock(b, flags);
            return 0;
        }
        net_bucket_unlock(b, flags);
    }
    return -ENFILE;
}

/*
 * Release the registry slot, and REPORT whether there was one.
 *
 * The return value is the whole point of this function's shape.  A socket that
 * owns a slot carries two references -- the creator's and the registry's --
 * and a socket that owns none carries exactly one.  Both kinds reach the same
 * two teardown paths (net_socket_close_file() and the accept() install-failure
 * rollback), because an AF_UNIX accepted child is never registered: see
 * net_unix_socket_connect(), which is why `grep -rn net_register_socket_locked
 * kernel/net/socket_unix.c` has no hits.  A teardown that unconditionally drops
 * "the registry's reference" therefore drops one reference too many for every
 * AF_UNIX connection, which frees the socket in the middle of its own teardown
 * and then keeps dereferencing it.
 *
 * That is not a new bug.  The pre-ledger net_socket_free() was a
 * __atomic_fetch_sub() compared against 1, so a free arriving at an already-zero
 * count read as -1, matched nothing, and returned -- the object was freed
 * exactly once, by accident rather than by design, and the use-after-free
 * survived because nothing read the freed memory in a way that faulted.  What
 * changed is that it is now visible: net_socket_free()'s CAS loop refuses a
 * non-positive count and the ref_magic canary rejects a socket that is not live,
 * so CONFIG_NET_REF_ASSERT=1 turns this into a panic on the first AF_UNIX close.
 * The ledger is what found it: the reference count had been walked by hand over
 * 54 sites and never once run.
 *
 * Returning "was there a slot" makes the coupling impossible to get wrong
 * again: the function that knows whether the reference exists is the one that
 * released the thing it was counted against.
 */
bool net_socket_unregister(net_socket_t *s) {
    if (!s)
        return false;
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
     *
     * reg_idx is read before the shard lock because choosing the shard is what
     * the read is for; `s` is the socket that owns the slot here (the caller is
     * its teardown), so the value cannot change under it.
     */
    int i = s->reg_idx;
    int b = net_socket_bucket(s);
    if (b < 0 || i < 0 || i >= NET_MAX_SOCKETS)
        return false;
    uint64_t flags = net_bucket_lock(b);
    uint64_t sflags = net_sock_lock(s);
    /* Released while the slot is still this socket's, because that is the key
     * the packet census is indexed by.  It writes pkt_bound_marked, which is
     * per-socket state, so it runs under the socket's lock like everything
     * else here. */
    net_packet_bound_release(s);
    bool released = a20_net_slot_release((void **)g_sockets, NET_MAX_SOCKETS,
                                         NET_SOCK_SLOTS_PER_BUCKET, i, s,
                                         &s->in_registry, &s->reg_idx,
                                         g_net_buckets[b].free_bits,
                                         NET_SOCK_BUCKET_WORDS,
                                         g_net_bh_pending,
                                         &g_net_bh_pending_count) != 0;
    net_sock_unlock(s, sflags);
    net_bucket_unlock(b, flags);
    /* The registry's own reference is released by the caller with one more
     * net_socket_free() -- only because this function returned true -- *after*
     * this function returns and every lock is dropped: it can free the socket,
     * and obj_cache_free() is not something to run with a shard lock held and
     * interrupts disabled. */
    return released;
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
