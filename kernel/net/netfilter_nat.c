/*
 * conntrack and NAT for the IPv4 data plane.
 *
 * See kernel/include/net/netfilter.h for the file header, the locking contract
 * and the HONEST BOUNDARIES.  The short version of the locking story: every
 * function in this file requires g_lwip_lock to be held.  That is already true
 * of both hook call sites and of a20_lwip_poll_timers_locked(), which is where
 * the sweeper runs from; the /proc entry points take the lock themselves
 * because a procfs access is a syscall and never runs under it.  That includes
 * this file's renderer: netfilter_format() acquires the lock before calling
 * netfilter_nat_format(), so nothing in this file may take it again.  A second
 * acquisition here self-deadlocks -- spin_lock_irqsave() is not recursive --
 * and it does so quietly enough to be mistaken for contention: the report is a
 * [LOCK-STALL] in netfilter_nat_format with owner=-1, because g_lwip_lock's
 * owner field is only maintained when CONFIG_NET_LOCK_ASSERT=1.
 *
 * Nothing here allocates either.  The table is a static array sized by
 * NET_PROFILE_CONNTRACK_ENTRIES and the translation is an in-place rewrite of
 * the frame the caller is already holding, so the whole module fits the
 * "no kmalloc under g_lwip_lock" rule in docs/net/network-lock-contract.md
 * without needing an escape hatch.
 */

#include "net/netfilter.h"
#include "net/netfilter_internal.h"
#include "net/netfilter_rewrite.h"
#include "net/net_profile.h"
#include "net/lwip_stack.h"
#include "core/lock.h"
#include "core/seqlock.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/errno.h"
#include "core/timer.h"

#define NET_CONNTRACK_MAX NET_PROFILE_CONNTRACK_ENTRIES
#define NET_CONNTRACK_BUCKETS NET_PROFILE_CONNTRACK_BUCKETS

/*
 * The profile comment in net_profile.h budgets "~64 bytes" per entry.  Pin it,
 * so a future field that blows the budget is a build error rather than a
 * silently larger .bss on a target that was sized against the old number.
 */
_Static_assert(sizeof(net_conntrack_entry_t) <= 64,
               "conntrack entry exceeds the per-tier budget in net_profile.h");
_Static_assert(NET_CONNTRACK_MAX < NET_CONNTRACK_NONE,
               "the chain sentinel must be outside the table index range");
_Static_assert((NET_CONNTRACK_MAX % NET_CONNTRACK_BUCKETS) == 0,
               "bucket count must divide the table size for the modulus hash");

static net_conntrack_entry_t g_ct[NET_CONNTRACK_MAX];
static uint16_t
    g_ct_head[NET_CONNTRACK_BUCKETS]; /* NET_CONNTRACK_NONE = empty */
static uint16_t g_ct_rhead[NET_CONNTRACK_BUCKETS]; /* reply-direction chain */
static unsigned g_ct_used;
static unsigned g_ct_sweep; /* sweeper resume point */
static int g_ct_enabled = 1;

static net_conntrack_stats_t g_ct_lifetime;
static unsigned g_ct_sweeps;

#define NET_CT_STATS_STRIDE 128

typedef struct {
    net_conntrack_stats_t stats;
    uint8_t pad[NET_CT_STATS_STRIDE - sizeof(net_conntrack_stats_t)];
} net_conntrack_stats_bank_t;

/* One cache-line-separated bank per direction, for the same reason the filter
 * keeps two: receive and transmit both touch a counter on every packet, and
 * sharing one line makes every lane contend for it. */
static net_conntrack_stats_bank_t g_ct_dir_stats[2];

/*
 * A slot is free exactly when its proto is 0.  NETFILTER_PROTO_ANY is a
 * matcher wildcard, never a wire protocol, and only TCP and UDP are tracked, so
 * a live entry always carries proto 6 or 17.  One test, used by the insert
 * scan, the eviction scan, the sweeper and the snapshot, cannot then be right
 * in one place and wrong in another the way a separate occupancy bitmap could.
 */
#define netfilter_ct_slot_used(i) (g_ct[i].proto != 0)

/*
 * The table is unusable before this runs.  It exists because the chain
 * sentinel is NET_CONNTRACK_NONE (0xFFFF) while a zeroed .bss gives every
 * bucket head the index 0 -- which is a *valid* slot, so a zeroed chain walks
 * 0 -> 0 -> 0 forever.  Relying on that being harmless until the first insert
 * is exactly the sort of "works until it does not" that costs an afternoon, so
 * the heads are initialised explicitly, once, from a20_lwip_init().
 */
void netfilter_conntrack_init(void) {
    for (unsigned b = 0; b < NET_CONNTRACK_BUCKETS; b++)
        g_ct_head[b] = NET_CONNTRACK_NONE;
    for (unsigned b = 0; b < NET_CONNTRACK_BUCKETS; b++)
        g_ct_rhead[b] = NET_CONNTRACK_NONE;
    memset(g_ct, 0, sizeof(g_ct));
    g_ct_used = 0;
    g_ct_sweep = 0;
    g_ct_enabled = 1;
}

/* ---------------------------------------------------------------------- time */

/*
 * Idle timeouts are written in milliseconds because that is how they are
 * specified, but the packet path must not pay a 64-bit divide per packet to
 * convert ticks.  The conversion is done once per timeout, lazily, and cached;
 * after that reading the clock is a plain tick read and every comparison is an
 * integer subtract.  Racing initialisers are harmless: both threads compute the
 * same value from the same constants, and the one writer that can change those
 * constants (netfilter_conntrack_set_timeouts) runs under g_lwip_lock, which
 * every reader here already holds.
 */
static uint32_t g_to_tcp_new;
static uint32_t g_to_tcp_established;
static uint32_t g_to_udp;
static int g_time_ready;

/*
 * Runtime overrides for the three millisecond constants, 0 = use the compiled-in
 * default.  They exist for the idle-timeout gate and nothing else: the shortest
 * compiled-in timeout is 30 seconds, which no test can afford to wait out, and
 * changing the constants per build would mean a second kernel image whose timeouts
 * are not the ones shipped.  Set from a procfs write under g_lwip_lock, so there
 * is no window in which the packet path sees a half-updated set.
 */
static uint32_t g_to_ms[3];

void netfilter_conntrack_set_timeouts(unsigned tcp_new_ms, unsigned tcp_est_ms,
                                      unsigned udp_ms) {
    g_to_ms[0] = tcp_new_ms;
    g_to_ms[1] = tcp_est_ms;
    g_to_ms[2] = udp_ms;
    /* Force the conversion to be redone on the next read.  Doing it here instead
     * would duplicate it, and the lazy path is the one that is already correct
     * for a caller that never reaches netfilter_ct_now(). */
    g_time_ready = 0;
}

static void netfilter_ct_time_init(void) {
    uint64_t hz = TICKS_PER_SEC;
    if (!hz)
        hz = 1000;
    uint32_t ms[3];
    ms[0] = g_to_ms[0] ? g_to_ms[0] : NET_CONNTRACK_TIMEOUT_TCP_NEW_MS;
    ms[1] = g_to_ms[1] ? g_to_ms[1] : NET_CONNTRACK_TIMEOUT_TCP_ESTABLISHED_MS;
    ms[2] = g_to_ms[2] ? g_to_ms[2] : NET_CONNTRACK_TIMEOUT_UDP_MS;
    g_to_tcp_new = (uint32_t)(ms[0] * hz / 1000);
    g_to_tcp_established = (uint32_t)(ms[1] * hz / 1000);
    g_to_udp = (uint32_t)(ms[2] * hz / 1000);
    /* A zero limit would mean "expires instantly" or "never expires"
     * depending on how the comparison is written; force one tick so that
     * neither reading is reachable. */
    if (!g_to_tcp_new)
        g_to_tcp_new = 1;
    if (!g_to_tcp_established)
        g_to_tcp_established = 1;
    if (!g_to_udp)
        g_to_udp = 1;
    g_time_ready = 1;
}

static uint32_t netfilter_ct_now(void) {
    if (!g_time_ready)
        netfilter_ct_time_init();
    return (uint32_t)timer_get_ticks();
}

/* --------------------------------------------------------------------- table */

static uint32_t netfilter_ct_hash(uint32_t src, uint32_t dst, uint16_t sport,
                                  uint16_t dport, uint8_t proto) {
    /* FNV-1a over the tuple, folded to a bucket.  The order is fixed (src
     * first) and deliberately *not* symmetric: a tuple and its reverse land in
     * different buckets, and netfilter_ct_find() compensates by probing both.
     * Making the hash symmetric instead would cost a swap on every packet to
     * save one probe on the reply. */
    uint32_t h = 2166136261u;
    const uint8_t words[13] = {(uint8_t)(src >> 24),
                               (uint8_t)(src >> 16),
                               (uint8_t)(src >> 8),
                               (uint8_t)src,
                               (uint8_t)(dst >> 24),
                               (uint8_t)(dst >> 16),
                               (uint8_t)(dst >> 8),
                               (uint8_t)dst,
                               (uint8_t)(sport >> 8),
                               (uint8_t)sport,
                               (uint8_t)(dport >> 8),
                               (uint8_t)dport,
                               proto};
    for (unsigned i = 0; i < sizeof(words); i++) {
        h ^= words[i];
        h *= 16777619u;
    }
    return h % NET_CONNTRACK_BUCKETS;
}

/* Returns the slot index, or NET_CONNTRACK_NONE.  *is_reverse distinguishes the
 * entry's own direction from the reply half. */
static unsigned netfilter_ct_find(uint32_t src, uint32_t dst, uint16_t sport,
                                  uint16_t dport, uint8_t proto,
                                  int *is_reverse) {
    unsigned b0 = (unsigned)netfilter_ct_hash(src, dst, sport, dport, proto);
    unsigned b1 = (unsigned)netfilter_ct_hash(dst, src, dport, sport, proto);
    for (unsigned pass = 0; pass < 2; pass++) {
        unsigned b = pass ? b1 : b0;
        for (unsigned idx = g_ct_head[b]; idx != NET_CONNTRACK_NONE;
             idx = g_ct[idx].hash_next) {
            const net_conntrack_entry_t *e = &g_ct[idx];
            if (e->proto != proto)
                continue;
            if (e->src_addr == src && e->dst_addr == dst &&
                e->src_port == sport && e->dst_port == dport) {
                *is_reverse = 0;
                return idx;
            }
            if (e->src_addr == dst && e->dst_addr == src &&
                e->src_port == dport && e->dst_port == sport) {
                *is_reverse = 1;
                return idx;
            }
        }
    }

    /* The reply-direction chain, which is the only place a translated reply can
     * be found.  One probe: the packet's tuple *is* the reply tuple, so the
     * swap is not tried here -- an entry whose reply tuple happens to equal the
     * swapped packet is not a match, and treating it as one would translate a
     * packet in the wrong direction. */
    for (unsigned idx = g_ct_rhead[b0]; idx != NET_CONNTRACK_NONE;
         idx = g_ct[idx].rhash_next) {
        const net_conntrack_entry_t *e = &g_ct[idx];
        uint32_t rsrc, rdst;
        uint16_t rsport, rdport;
        if (e->proto != proto)
            continue;
        netfilter_ct_reply_tuple(e, &rsrc, &rdst, &rsport, &rdport);
        if (rsrc == src && rdst == dst && rsport == sport && rdport == dport) {
            *is_reverse = 1;
            return idx;
        }
    }
    return NET_CONNTRACK_NONE;
}

/* Unlink from one chain of `head`, comparing the link field named by `link`. */
static void netfilter_ct_unlink_from(uint16_t *head, unsigned b, unsigned idx,
                                     int reply) {
    unsigned prev = NET_CONNTRACK_NONE;
    for (unsigned i = head[b]; i != NET_CONNTRACK_NONE;) {
        uint16_t next = reply ? g_ct[i].rhash_next : g_ct[i].hash_next;
        if (i != idx) {
            prev = i;
            i = next;
            continue;
        }
        if (prev == NET_CONNTRACK_NONE)
            head[b] = next;
        else if (reply)
            g_ct[prev].rhash_next = next;
        else
            g_ct[prev].hash_next = next;
        if (reply)
            g_ct[i].rhash_next = NET_CONNTRACK_NONE;
        else
            g_ct[i].hash_next = NET_CONNTRACK_NONE;
        return;
    }
}

static void netfilter_ct_unlink(unsigned idx) {
    unsigned bf = (unsigned)netfilter_ct_hash(
        g_ct[idx].src_addr, g_ct[idx].dst_addr, g_ct[idx].src_port,
        g_ct[idx].dst_port, g_ct[idx].proto);
    netfilter_ct_unlink_from(g_ct_head, bf, idx, 0);

    uint32_t rsrc, rdst;
    uint16_t rsport, rdport;
    netfilter_ct_reply_tuple(&g_ct[idx], &rsrc, &rdst, &rsport, &rdport);
    unsigned br = (unsigned)netfilter_ct_hash(rsrc, rdst, rsport, rdport,
                                              g_ct[idx].proto);
    netfilter_ct_unlink_from(g_ct_rhead, br, idx, 1);
}

/* Least recently used: the live entry with the smallest last_ms.  Ties are
 * broken by index, which only matters when two entries were touched in the same
 * tick -- either is then a defensible victim. */
static unsigned netfilter_ct_lru(void) {
    unsigned best = NET_CONNTRACK_NONE;
    uint32_t best_ms = 0;
    for (unsigned i = 0; i < NET_CONNTRACK_MAX; i++) {
        if (!netfilter_ct_slot_used(i))
            continue;
        if (best == NET_CONNTRACK_NONE || g_ct[i].last_ms < best_ms ||
            (g_ct[i].last_ms == best_ms && i < best)) {
            best = i;
            best_ms = g_ct[i].last_ms;
        }
    }
    return best;
}

static int netfilter_ct_insert(net_conntrack_entry_t *entry) {
    if (!entry)
        return -EINVAL;
    entry->last_ms = netfilter_ct_now();
    entry->hash_next = NET_CONNTRACK_NONE;

    unsigned slot;
    int was_full = g_ct_used >= NET_CONNTRACK_MAX;
    if (!was_full) {
        for (slot = 0; slot < NET_CONNTRACK_MAX; slot++)
            if (!netfilter_ct_slot_used(slot))
                break;
        if (slot == NET_CONNTRACK_MAX)
            return -ENOSPC; /* the counter and the slots disagreed */
    } else {
        /* At capacity the table degrades into a bounded LRU cache rather than
         * a failure: a busy router keeps its newest flows and forgets its
         * oldest, which is the right trade for a table this small. */
        slot = netfilter_ct_lru();
        if (slot == NET_CONNTRACK_NONE)
            return -ENOSPC;
        netfilter_ct_unlink(slot);
        __atomic_fetch_add(&g_ct_lifetime.evicted, 1, __ATOMIC_RELAXED);
    }

    unsigned b = (unsigned)netfilter_ct_hash(entry->src_addr, entry->dst_addr,
                                             entry->src_port, entry->dst_port,
                                             entry->proto);
    uint32_t rsrc, rdst;
    uint16_t rsport, rdport;
    netfilter_ct_reply_tuple(entry, &rsrc, &rdst, &rsport, &rdport);
    unsigned rb =
        (unsigned)netfilter_ct_hash(rsrc, rdst, rsport, rdport, entry->proto);
    g_ct[slot] = *entry;
    g_ct[slot].hash_next = g_ct_head[b];
    g_ct_head[b] = (uint16_t)slot;
    g_ct[slot].rhash_next = g_ct_rhead[rb];
    g_ct_rhead[rb] = (uint16_t)slot;
    if (!was_full)
        g_ct_used++;
    __atomic_fetch_add(&g_ct_lifetime.created, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ct_lifetime.tracked, g_ct_used, __ATOMIC_RELAXED);
    return 0;
}

int netfilter_conntrack_count(void) {
    return (int)g_ct_used;
}

unsigned netfilter_conntrack_sweeps(void) {
    return __atomic_load_n(&g_ct_sweeps, __ATOMIC_RELAXED);
}

int netfilter_conntrack_lru_victim(void) {
    unsigned slot = netfilter_ct_lru();
    return (slot == NET_CONNTRACK_NONE) ? -1 : (int)g_ct[slot].src_port;
}

int netfilter_conntrack_inject(uint32_t src, uint32_t dst, uint16_t sport,
                               uint16_t dport, uint8_t proto, uint8_t state) {
    /* Only what the packet path would have created: an untracked flow is a UDP
     * or TCP tuple in one of the two real states.  Anything else would let a
     * gate assert against a table shape the data plane cannot produce. */
    if (proto != NETFILTER_PROTO_TCP && proto != NETFILTER_PROTO_UDP)
        return -EINVAL;
    if (state != NET_CONNTRACK_NEW && state != NET_CONNTRACK_ESTABLISHED)
        return -EINVAL;
    net_conntrack_entry_t e;
    memset(&e, 0, sizeof(e));
    e.src_addr = src;
    e.dst_addr = dst;
    e.src_port = sport;
    e.dst_port = dport;
    e.proto = proto;
    e.state = state;
    e.dir = NETFILTER_DIR_OUT;
    /* Through the real insert: chaining, capacity, LRU victim choice and both
     * lifetime counters all happen inside it. */
    return netfilter_ct_insert(&e);
}

int netfilter_conntrack_snapshot(net_conntrack_entry_t *out, unsigned max) {
    if (!out || !max)
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < NET_CONNTRACK_MAX && n < max; i++)
        if (netfilter_ct_slot_used(i))
            out[n++] = g_ct[i];
    return (int)n;
}

void netfilter_conntrack_flush(void) {
    for (unsigned b = 0; b < NET_CONNTRACK_BUCKETS; b++)
        g_ct_head[b] = NET_CONNTRACK_NONE;
    for (unsigned b = 0; b < NET_CONNTRACK_BUCKETS; b++)
        g_ct_rhead[b] = NET_CONNTRACK_NONE;
    memset(g_ct, 0, sizeof(g_ct));
    g_ct_used = 0;
    g_ct_sweep = 0;
    /*
     * `tracked` follows the table; the lifetime counters deliberately do not.
     * Flushing is an operator action, and a test that wants to observe
     * reclamation has to be able to flush first without erasing the evidence it
     * came to read.
     */
    __atomic_store_n(&g_ct_lifetime.tracked, 0, __ATOMIC_RELAXED);
}

void netfilter_conntrack_get_stats(net_conntrack_stats_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    for (unsigned d = 0; d < 2; d++) {
        const net_conntrack_stats_t *s = &g_ct_dir_stats[d].stats;
        out->packets += __atomic_load_n(&s->packets, __ATOMIC_RELAXED);
        out->bytes += __atomic_load_n(&s->bytes, __ATOMIC_RELAXED);
    }
    out->tracked = g_ct_used;
    out->created = __atomic_load_n(&g_ct_lifetime.created, __ATOMIC_RELAXED);
    out->evicted = __atomic_load_n(&g_ct_lifetime.evicted, __ATOMIC_RELAXED);
    out->timeout = __atomic_load_n(&g_ct_lifetime.timeout, __ATOMIC_RELAXED);
}

void netfilter_conntrack_set_enabled(int on) {
    /*
     * Entries are kept when tracking is switched off.  Deleting them would
     * silently break every flow currently relying on a NAT binding, and
     * "stop tracking" is not obviously a request to tear down live state.
     */
    g_ct_enabled = on ? 1 : 0;
}

int netfilter_conntrack_enabled(void) {
    return g_ct_enabled;
}

unsigned netfilter_conntrack_expire(unsigned max_scan) {
    uint32_t now = netfilter_ct_now();
    unsigned scanned = 0, freed = 0;

    if (!max_scan)
        max_scan = NET_CONNTRACK_MAX;

    /* How many times the sweep has run at all.  The timeout counter says what
     * the sweeper reclaimed; this says whether it ran, which is the difference
     * between "nothing was idle" and "nothing ever looked". */
    __atomic_fetch_add(&g_ct_sweeps, 1, __ATOMIC_RELAXED);

    /*
     * Resume where the previous tick stopped rather than restarting at 0: a
     * full table would otherwise need NET_CONNTRACK_MAX comparisons on every
     * timer tick to expire nothing, and this runs on the CPU 0 timer interrupt
     * (kernel_progress_timer_tick -> a20_lwip_poll_timers_locked).
     */
    for (unsigned step = 0; step < NET_CONNTRACK_MAX && scanned < max_scan;
         step++) {
        unsigned i = (g_ct_sweep + step) % NET_CONNTRACK_MAX;
        scanned++;
        if (!netfilter_ct_slot_used(i))
            continue;
        uint32_t limit;
        if (g_ct[i].proto == NETFILTER_PROTO_UDP)
            limit = g_to_udp;
        else if (g_ct[i].state == NET_CONNTRACK_ESTABLISHED)
            limit = g_to_tcp_established;
        else
            limit = g_to_tcp_new;
        /* Unsigned subtract: correct across the tick counter's wrap, which a
         * `now > last + limit` comparison gets wrong once every ~49 days on a
         * 1 kHz tick. */
        if ((uint32_t)(now - g_ct[i].last_ms) < limit)
            continue;
        netfilter_ct_unlink(i);
        memset(&g_ct[i], 0, sizeof(g_ct[i]));
        if (g_ct_used)
            g_ct_used--;
        freed++;
        __atomic_fetch_add(&g_ct_lifetime.timeout, 1, __ATOMIC_RELAXED);
    }
    g_ct_sweep = (g_ct_sweep + scanned) % NET_CONNTRACK_MAX;
    __atomic_store_n(&g_ct_lifetime.tracked, g_ct_used, __ATOMIC_RELAXED);
    return freed;
}

/* ------------------------------------------------------------------ NAT rules */

static net_nat_rule_t g_nat[NETFILTER_MAX_NAT_RULES];
static unsigned g_nat_count;
static seqlock_t g_nat_seq = SEQLOCK_INIT;

int netfilter_nat_rule_count(void) {
    return (int)__atomic_load_n(&g_nat_count, __ATOMIC_ACQUIRE);
}

/*
 * The NAT table faces the same problem the filter table does -- written from
 * procfs, read from a packet path holding a different lock -- so it reuses the
 * same answer: writers bracket with g_nat_seq under g_netfilter_lock, readers
 * stay wait-free.  It cannot use g_lwip_lock, because these writers are
 * procfs writes that do not hold it.  Two independent seqlocks over two
 * independent tables is the price of not putting a second lock in the packet
 * path.
 */
int netfilter_nat_add_rule(const net_nat_rule_t *rule) {
    if (!rule)
        return -EINVAL;
    if (rule->nat != NET_NAT_SNAT && rule->nat != NET_NAT_MASQUERADE &&
        rule->nat != NET_NAT_DNAT)
        return -EINVAL;
    if (rule->dir != NETFILTER_DIR_IN && rule->dir != NETFILTER_DIR_OUT)
        return -EINVAL;
    /* A masquerade with an explicit address would be ambiguous, and a plain
     * snat/dnat without one would match packets and then do nothing; both are
     * rejected here as well as at parse time, because this entry point is
     * reachable without going through the parser. */
    if ((rule->nat == NET_NAT_SNAT || rule->nat == NET_NAT_DNAT) &&
        rule->nat_addr == NETFILTER_NO_ADDR)
        return -EINVAL;
    if (rule->nat == NET_NAT_MASQUERADE && rule->nat_addr != NETFILTER_NO_ADDR)
        return -EINVAL;
    /* DNAT is an input-hook translation; translating on the way out would be
     * a different feature that this does not implement. */
    if (rule->nat == NET_NAT_DNAT && rule->dir != NETFILTER_DIR_IN)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    if (g_nat_count >= NETFILTER_MAX_NAT_RULES) {
        spin_unlock_irqrestore(&g_netfilter_lock, flags);
        return -ENOSPC;
    }
    seqlock_write_begin(&g_nat_seq);
    g_nat[g_nat_count] = *rule;
    g_nat_count++;
    seqlock_write_end(&g_nat_seq);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return (int)(g_nat_count - 1);
}

int netfilter_nat_del_rule(unsigned index) {
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    if (index >= g_nat_count) {
        spin_unlock_irqrestore(&g_netfilter_lock, flags);
        return -EINVAL;
    }
    seqlock_write_begin(&g_nat_seq);
    for (unsigned i = index; i + 1 < g_nat_count; i++)
        g_nat[i] = g_nat[i + 1];
    g_nat_count--;
    memset(&g_nat[g_nat_count], 0, sizeof(g_nat[0]));
    seqlock_write_end(&g_nat_seq);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return 0;
}

void netfilter_nat_reset(void) {
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    seqlock_write_begin(&g_nat_seq);
    memset(g_nat, 0, sizeof(g_nat));
    g_nat_count = 0;
    seqlock_write_end(&g_nat_seq);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
}

int netfilter_nat_parse_rule(const char *line, size_t len,
                             net_nat_rule_t *out) {
    if (!line || !out)
        return -EINVAL;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;

    memset(out, 0, sizeof(*out));
    out->proto = NETFILTER_PROTO_ANY;
    out->nat = NET_NAT_NONE;
    out->src_addr = NETFILTER_NO_ADDR;
    out->dst_addr = NETFILTER_NO_ADDR;
    out->nat_addr = NETFILTER_NO_ADDR;
    out->src_port = NETFILTER_NO_PORT;
    out->dst_port = NETFILTER_NO_PORT;

    if (len == 0)
        return -EINVAL;

    size_t i = 0;
    if (len >= 2 && strncmp(line, "in", 2) == 0) {
        out->dir = NETFILTER_DIR_IN;
        i = 2;
    } else if (len >= 3 && strncmp(line, "out", 3) == 0) {
        out->dir = NETFILTER_DIR_OUT;
        i = 3;
    } else {
        return -EINVAL;
    }
    if (i < len && line[i] == ' ')
        i++;

    while (i < len) {
        size_t klen = 0;
        while (i + klen < len && line[i + klen] != '=')
            klen++;
        if (i + klen >= len)
            break; /* trailing garbage, as in the filter */
        const char *key = line + i;
        const char *val = line + i + klen + 1;
        size_t vlen = 0;
        while (i + klen + 1 + vlen < len && line[i + klen + 1 + vlen] != ' ' &&
               line[i + klen + 1 + vlen] != '\t')
            vlen++;

        if (klen == 5 && strncmp(key, "proto", 5) == 0) {
            if (vlen == 3 && strncmp(val, "any", 3) == 0) {
                out->proto = NETFILTER_PROTO_ANY;
            } else if (vlen == 4 && strncmp(val, "icmp", 4) == 0) {
                out->proto = NETFILTER_PROTO_ICMP;
            } else if (vlen == 3 && strncmp(val, "tcp", 3) == 0) {
                out->proto = NETFILTER_PROTO_TCP;
            } else if (vlen == 3 && strncmp(val, "udp", 3) == 0) {
                out->proto = NETFILTER_PROTO_UDP;
            } else if (vlen >= 1 && vlen <= 3 && val[0] >= '0' &&
                       val[0] <= '9') {
                uint32_t p = 0;
                for (size_t k = 0; k < vlen; k++) {
                    if (val[k] < '0' || val[k] > '9')
                        return -EINVAL;
                    p = p * 10 + (uint32_t)(val[k] - '0');
                }
                if (p > 255)
                    return -EINVAL;
                out->proto = (uint8_t)p;
            } else {
                return -EINVAL;
            }
        } else if (klen == 3 && strncmp(key, "src", 3) == 0) {
            if (vlen == 3 && strncmp(val, "any", 3) == 0) {
                out->src_addr = NETFILTER_NO_ADDR;
            } else if (!netfilter_ipv4_from_str(val, vlen, &out->src_addr)) {
                return -EINVAL;
            }
        } else if (klen == 3 && strncmp(key, "dst", 3) == 0) {
            if (vlen == 3 && strncmp(val, "any", 3) == 0) {
                out->dst_addr = NETFILTER_NO_ADDR;
            } else if (!netfilter_ipv4_from_str(val, vlen, &out->dst_addr)) {
                return -EINVAL;
            }
        } else if ((klen == 5 && strncmp(key, "sport", 5) == 0) ||
                   (klen == 5 && strncmp(key, "dport", 5) == 0)) {
            uint16_t *dst = klen == 5 && strncmp(key, "sport", 5) == 0
                                ? &out->src_port
                                : &out->dst_port;
            if (vlen == 3 && strncmp(val, "any", 3) == 0) {
                *dst = NETFILTER_NO_PORT;
            } else {
                uint32_t p = 0;
                for (size_t k = 0; k < vlen; k++) {
                    if (val[k] < '0' || val[k] > '9')
                        return -EINVAL;
                    p = p * 10 + (uint32_t)(val[k] - '0');
                    if (p > NETFILTER_NO_PORT)
                        return -EINVAL;
                }
                *dst = (uint16_t)p;
            }
        } else if (klen == 6 && strncmp(key, "action", 6) == 0) {
            if (vlen == 4 && strncmp(val, "snat", 4) == 0) {
                out->nat = NET_NAT_SNAT;
            } else if (vlen == 10 && strncmp(val, "masquerade", 10) == 0) {
                out->nat = NET_NAT_MASQUERADE;
            } else if (vlen == 4 && strncmp(val, "dnat", 4) == 0) {
                out->nat = NET_NAT_DNAT;
            } else {
                return -EINVAL;
            }
        } else if (klen == 2 && strncmp(key, "to", 2) == 0) {
            if (!netfilter_ipv4_from_str(val, vlen, &out->nat_addr))
                return -EINVAL;
        } else if (klen == 6 && strncmp(key, "toport", 6) == 0) {
            uint32_t p = 0;
            for (size_t k = 0; k < vlen; k++) {
                if (val[k] < '0' || val[k] > '9')
                    return -EINVAL;
                p = p * 10 + (uint32_t)(val[k] - '0');
                if (p > NETFILTER_NO_PORT)
                    return -EINVAL;
            }
            out->nat_port = (uint16_t)p;
        } else {
            return -EINVAL;
        }

        i += klen + 1 + vlen;
        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            i++;
    }

    /* Refuse a rule that would match packets and then do nothing, or that names
     * two conflicting targets.  Rejecting at parse time means the /proc write
     * fails loudly instead of installing a rule whose only effect is a matched
     * counter that misleads whoever reads it back. */
    if (out->nat == NET_NAT_NONE)
        return -EINVAL;
    if (out->nat == NET_NAT_DNAT && out->dir != NETFILTER_DIR_IN)
        return -EINVAL;
    if (out->nat == NET_NAT_MASQUERADE) {
        if (out->nat_addr != NETFILTER_NO_ADDR || out->nat_port)
            return -EINVAL;
    } else if (out->nat_addr == NETFILTER_NO_ADDR) {
        return -EINVAL;
    }
    return 0;
}

/* ---------------------------------------------------------------- translation */

static int netfilter_nat_pick(const netfilter_frame_t *pkt, netfilter_dir_t dir,
                              net_nat_rule_t *out) {
    /*
     * First match wins, the same rule the filter uses, so one mental model
     * covers both tables.  The matched counter is bumped inside the seqlock
     * bracket for the reason spelled out in netfilter.c: after the bracket a
     * retry could charge a different rule that slid into the index.
     */
    int found = -1;
    for (unsigned attempt = 0; attempt < SEQLOCK_READ_ATTEMPTS; attempt++) {
        unsigned seq0 = seqlock_read_begin(&g_nat_seq);
        if (seq0 == 0u)
            continue;
        unsigned n = __atomic_load_n(&g_nat_count, __ATOMIC_RELAXED);
        int hit = -1;
        for (unsigned i = 0; i < n; i++) {
            net_nat_rule_t *r = &g_nat[i];
            if (r->dir != dir)
                continue;
            if (r->proto != NETFILTER_PROTO_ANY && r->proto != pkt->proto)
                continue;
            if (r->src_addr != NETFILTER_NO_ADDR &&
                r->src_addr != pkt->src_addr)
                continue;
            if (r->dst_addr != NETFILTER_NO_ADDR &&
                r->dst_addr != pkt->dst_addr)
                continue;
            if (r->src_port != NETFILTER_NO_PORT &&
                r->src_port != pkt->src_port)
                continue;
            if (r->dst_port != NETFILTER_NO_PORT &&
                r->dst_port != pkt->dst_port)
                continue;
            hit = (int)i;
            break;
        }
        if (seqlock_read_retry(&g_nat_seq, seq0))
            continue;
        found = hit;
        if (found >= 0)
            *out = g_nat[found];
        break;
    }
    return found >= 0;
}

void netfilter_conntrack_process(uint8_t *frame, size_t len, int net_idx,
                                 netfilter_dir_t dir,
                                 const netfilter_frame_t *pkt) {
    if (!frame || !pkt || len < 14)
        return;

    /*
     * Only TCP and UDP are tracked.  Everything else -- ICMP above all -- is
     * passed through untranslated and untracked, which is stated in the
     * header's HONEST BOUNDARIES rather than left for a reader to discover.
     */
    if (pkt->proto != NETFILTER_PROTO_TCP && pkt->proto != NETFILTER_PROTO_UDP)
        return;

    /* A non-first fragment carries no ports, so there is no tuple to key on and
     * no header to rewrite against.  Passing it through untranslated is the
     * honest choice: the alternative -- track the flow but translate nothing --
     * would make the entry look stateful while its translation is incomplete. */
    if (!pkt->has_ports || pkt->l4_off == 0)
        return;

    net_conntrack_stats_t *st =
        &g_ct_dir_stats[dir == NETFILTER_DIR_IN ? 0 : 1].stats;
    __atomic_fetch_add(&st->packets, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st->bytes, len, __ATOMIC_RELAXED);

    int is_reverse = 0;
    unsigned idx =
        netfilter_ct_find(pkt->src_addr, pkt->dst_addr, pkt->src_port,
                          pkt->dst_port, pkt->proto, &is_reverse);

    if (idx != NET_CONNTRACK_NONE) {
        net_conntrack_entry_t *e = &g_ct[idx];
        e->last_ms = netfilter_ct_now();
        e->packets++;
        __atomic_fetch_add(&e->bytes, len, __ATOMIC_RELAXED);

        if (is_reverse)
            e->state = NET_CONNTRACK_ESTABLISHED;
        else if (pkt->proto == NETFILTER_PROTO_TCP && (pkt->tcp_flags & 0x10) &&
                 !(pkt->tcp_flags & 0x02))
            e->state = NET_CONNTRACK_ESTABLISHED;

        /*
         * Every later packet of the flow is translated from the entry, not from
         * the rules.  That is the whole reason the translation is recorded: a
         * NAT rule may have been deleted, reordered or replaced since the flow
         * was first seen, and re-evaluating it would either break the reply or
         * silently move a live binding.
         *
         * "Later packet" includes the *same direction* as the first one, which
         * is not a hypothetical: a TCP SYN that is retransmitted arrives as a
         * conntrack hit travelling the forward way, and translating only the
         * reverse way leaves that retransmission addressed to the port nothing
         * is listening on.  lwIP answers it with a RST, and the peer's
         * connection dies to a packet the peer never got wrong.  That is
         * exactly what the first end-to-end DNAT run did: the first SYN was
         * translated, the second arrived untranslated and drew the RST.
         *
         * The four rewrites are the inverse of each other, so both directions
         * come out of the same pair of fields: the entry keeps the tuple as it
         * appeared before any translation (e->src_addr and friends) plus the
         * translated values it applied (e->nat_src_addr and friends).
         */
        if (e->nat == NET_NAT_SNAT || e->nat == NET_NAT_MASQUERADE) {
            if (is_reverse) {
                /* Reply arriving: its destination is the translated source, and
                 * the original source is where that destination has to go. */
                netfilter_set_addr(frame, len, pkt, pkt->src_addr, e->src_addr);
                netfilter_set_port(frame, len, pkt, 0, e->src_port);
            } else {
                netfilter_set_addr(frame, len, pkt, e->nat_src_addr,
                                   pkt->dst_addr);
                netfilter_set_port(frame, len, pkt, 1, e->nat_src_port);
            }
        } else if (e->nat == NET_NAT_DNAT) {
            if (is_reverse) {
                /* Reply leaving: its source is the translated destination, and
                 * the original destination is where that source has to go. */
                netfilter_set_addr(frame, len, pkt, e->dst_addr, pkt->dst_addr);
                netfilter_set_port(frame, len, pkt, 1, e->dst_port);
            } else {
                netfilter_set_addr(frame, len, pkt, pkt->src_addr,
                                   e->nat_dst_addr);
                netfilter_set_port(frame, len, pkt, 0, e->nat_dst_port);
            }
        }
        return;
    }

    if (!g_ct_enabled)
        return;

    net_conntrack_entry_t e;
    memset(&e, 0, sizeof(e));
    e.src_addr = pkt->src_addr;
    e.dst_addr = pkt->dst_addr;
    e.src_port = pkt->src_port;
    e.dst_port = pkt->dst_port;
    e.proto = pkt->proto;
    e.dir = (uint8_t)dir;
    e.state = NET_CONNTRACK_NEW;
    e.nat = NET_NAT_NONE;
    e.packets = 1;
    e.bytes = len;

    net_nat_rule_t rule;
    if (netfilter_nat_pick(pkt, dir, &rule)) {
        if (rule.nat == NET_NAT_DNAT && dir == NETFILTER_DIR_IN) {
            e.nat = NET_NAT_DNAT;
            e.nat_dst_addr = rule.nat_addr;
            e.nat_dst_port = rule.nat_port;
            netfilter_set_addr(frame, len, pkt, pkt->src_addr, rule.nat_addr);
            netfilter_set_port(frame, len, pkt, 0, rule.nat_port);
        } else if ((rule.nat == NET_NAT_SNAT ||
                    rule.nat == NET_NAT_MASQUERADE) &&
                   dir == NETFILTER_DIR_OUT) {
            uint32_t target = rule.nat == NET_NAT_MASQUERADE
                                  ? netfilter_masq_addr(net_idx)
                                  : rule.nat_addr;
            if (target) {
                e.nat = rule.nat;
                e.nat_src_addr = target;
                e.nat_src_port = rule.nat_port;
                netfilter_set_addr(frame, len, pkt, target, pkt->dst_addr);
                netfilter_set_port(frame, len, pkt, 1, rule.nat_port);
            }
        }
    }

    /*
     * Tracking is best-effort by design: if the insert fails the packet has
     * already been translated (or not) and must not be dropped or have its
     * translation rolled back because the bookkeeping had nowhere to go.  The
     * only way to reach that branch is a pathological configuration, since a
     * full table evicts instead of failing.
     */
    (void)netfilter_ct_insert(&e);
}

/* -------------------------------------------------------------------- format */

static const char *netfilter_nat_name(uint8_t nat) {
    switch (nat) {
        case NET_NAT_SNAT:
            return "snat";
        case NET_NAT_MASQUERADE:
            return "masquerade";
        case NET_NAT_DNAT:
            return "dnat";
        default:
            return "none";
    }
}

/* snprintf reports what it *would* have written; letting the cursor grow past
 * bufsz would make every later bufsz - off underflow, so it is clamped here and
 * the caller's "off >= bufsz" test means "stop". */
static size_t netfilter_append(char *buf, size_t bufsz, size_t off,
                               const char *s) {
    while (*s && off < bufsz)
        buf[off++] = *s++;
    return off;
}

#define NETFILTER_EMIT(...)                                                    \
    do {                                                                       \
        char _l[256];                                                          \
        snprintf(_l, sizeof(_l), __VA_ARGS__);                                 \
        off = netfilter_append(buf, bufsz, off, _l);                           \
    } while (0)

void netfilter_nat_format(char *buf, size_t bufsz, size_t off) {
    if (!buf)
        return;
    net_conntrack_stats_t cs;
    netfilter_conntrack_get_stats(&cs);

    NETFILTER_EMIT("conntrack: %s\n", g_ct_enabled ? "on" : "off");
    NETFILTER_EMIT("ct_capacity: %u\n", (unsigned)NET_CONNTRACK_MAX);
    NETFILTER_EMIT("ct_tracked: %llu\n", (unsigned long long)cs.tracked);
    NETFILTER_EMIT("ct_created: %llu\n", (unsigned long long)cs.created);
    NETFILTER_EMIT("ct_evicted: %llu\n", (unsigned long long)cs.evicted);
    NETFILTER_EMIT("ct_timeout: %llu\n", (unsigned long long)cs.timeout);
    NETFILTER_EMIT("ct_lru_victim: %d\n", netfilter_conntrack_lru_victim());
    NETFILTER_EMIT("ct_sweeps: %u\n", netfilter_conntrack_sweeps());
    NETFILTER_EMIT("ct_packets: %llu\n", (unsigned long long)cs.packets);
    NETFILTER_EMIT("nat_rules: %u\n", g_nat_count);

    for (unsigned i = 0; i < g_nat_count && off < bufsz; i++) {
        const net_nat_rule_t *r = &g_nat[i];
        char na[16] = "-";
        if (r->nat != NET_NAT_MASQUERADE)
            netfilter_ipv4_to_str(r->nat_addr, na, sizeof(na));
        NETFILTER_EMIT(
            "nat %u: %s proto=%u dport=%u action=%s to=%s "
            "matched=%llu\n",
            i, r->dir == NETFILTER_DIR_IN ? "in" : "out", r->proto, r->dst_port,
            netfilter_nat_name(r->nat), na,
            (unsigned long long)__atomic_load_n(&r->matched, __ATOMIC_RELAXED));
    }

    /*
     * Entries, bounded by both a row cap and what is left of the 4096 bytes
     * procfs hands the renderer.  A truncation notice is printed rather than a
     * silently short list, because a short list that does not say so is
     * indistinguishable from a quiet system.
     */
    enum { NET_CT_FORMAT_ROWS = 8 };
    net_conntrack_entry_t rows[NET_CT_FORMAT_ROWS];
    int n = netfilter_conntrack_snapshot(rows, NET_CT_FORMAT_ROWS);
    for (int i = 0; i < n && off < bufsz; i++) {
        const net_conntrack_entry_t *e = &rows[i];
        NETFILTER_EMIT(
            "ct %d: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u proto=%u "
            "state=%s nat=%s packets=%llu\n",
            i, (e->src_addr >> 24) & 0xff, (e->src_addr >> 16) & 0xff,
            (e->src_addr >> 8) & 0xff, e->src_addr & 0xff, e->src_port,
            (e->dst_addr >> 24) & 0xff, (e->dst_addr >> 16) & 0xff,
            (e->dst_addr >> 8) & 0xff, e->dst_addr & 0xff, e->dst_port,
            e->proto,
            e->state == NET_CONNTRACK_ESTABLISHED ? "established" : "new",
            netfilter_nat_name(e->nat), (unsigned long long)e->packets);
    }
    if (g_ct_used > (unsigned)n)
        NETFILTER_EMIT("ct ... %u more\n", g_ct_used - (unsigned)n);
}

#undef NETFILTER_EMIT