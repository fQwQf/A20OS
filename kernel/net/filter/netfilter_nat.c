/* IPv4 conntrack/NAT storage and kernel primitives. Rust transactions acquire
 * g_ct_lock through CtGuard, independently of any protocol backend's locking.
 * Ordering: backend lock or rule-writer lock -> CT lock; never the reverse.
 * No allocation, backend lock acquisition, blocking waits or external pointer ownership
 * transfers occur while holding the CT lock. */

#include "net/netfilter.h"
#include "net/filter/netfilter_internal.h"
#include "net/netfilter_rewrite.h"
#include "net/profile.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/errno.h"
#include "core/timer.h"

#include "net/netfilter_nat_state.h"

/* The host harness links this file without netfilter.c.  The strong
 * definition in netfilter.c wins in the kernel; this fallback keeps the
 * conntrack state machine independently testable. */
__attribute__((weak)) void netfilter_busy_refresh(void) {}
__attribute__((weak)) void netfilter_busy_force(int on) { (void)on; }

static spinlock_t g_ct_lock = SPINLOCK_INIT;

uint64_t a20_netfilter_ct_lock(void) { return spin_lock_irqsave(&g_ct_lock); }
void a20_netfilter_ct_unlock(uint64_t flags) {
    spin_unlock_irqrestore(&g_ct_lock, flags);
}

/*
 * The Rust mirror of these three structs is field-for-field, so every offset is
 * pinned here: a change to net/netfilter.h that moves a field must fail this
 * build rather than corrupt the state machine on the other side.
 */
_Static_assert(__builtin_offsetof(net_conntrack_entry_t, dst_addr) == 4 &&
               __builtin_offsetof(net_conntrack_entry_t, src_port) == 8 &&
               __builtin_offsetof(net_conntrack_entry_t, dst_port) == 10 &&
               __builtin_offsetof(net_conntrack_entry_t, proto) == 12 &&
               __builtin_offsetof(net_conntrack_entry_t, state) == 13 &&
               __builtin_offsetof(net_conntrack_entry_t, nat) == 14 &&
               __builtin_offsetof(net_conntrack_entry_t, dir) == 15 &&
               __builtin_offsetof(net_conntrack_entry_t, nat_src_addr) == 16 &&
               __builtin_offsetof(net_conntrack_entry_t, nat_dst_addr) == 20 &&
               __builtin_offsetof(net_conntrack_entry_t, nat_src_port) == 24 &&
               __builtin_offsetof(net_conntrack_entry_t, nat_dst_port) == 26 &&
               __builtin_offsetof(net_conntrack_entry_t, last_ms) == 28 &&
               __builtin_offsetof(net_conntrack_entry_t, packets) == 32 &&
               __builtin_offsetof(net_conntrack_entry_t, bytes) == 40 &&
               __builtin_offsetof(net_conntrack_entry_t, hash_next) == 48 &&
               __builtin_offsetof(net_conntrack_entry_t, rhash_next) == 50 &&
               sizeof(net_conntrack_entry_t) == 56,
               "net_conntrack_entry_t layout changed: update CtView in netfilter_nat.rs");
_Static_assert(__builtin_offsetof(netfilter_frame_t, dst_addr) == 4 &&
               __builtin_offsetof(netfilter_frame_t, src_port) == 8 &&
               __builtin_offsetof(netfilter_frame_t, dst_port) == 10 &&
               __builtin_offsetof(netfilter_frame_t, proto) == 12 &&
               __builtin_offsetof(netfilter_frame_t, has_ports) == 13 &&
               __builtin_offsetof(netfilter_frame_t, ip_off) == 14 &&
               __builtin_offsetof(netfilter_frame_t, l4_off) == 16 &&
               __builtin_offsetof(netfilter_frame_t, tcp_flags) == 18 &&
               __builtin_offsetof(netfilter_frame_t, icmp_off) == 20 &&
               __builtin_offsetof(netfilter_frame_t, icmp_type) == 22 &&
               __builtin_offsetof(netfilter_frame_t, icmp_id) == 24 &&
               sizeof(netfilter_frame_t) == 28,
               "netfilter_frame_t layout changed: update Frame in netfilter_nat.rs");
_Static_assert(__builtin_offsetof(net_nat_rule_t, src_port) == 4 &&
               __builtin_offsetof(net_nat_rule_t, dst_port) == 6 &&
               __builtin_offsetof(net_nat_rule_t, src_addr) == 8 &&
               __builtin_offsetof(net_nat_rule_t, dst_addr) == 12 &&
               __builtin_offsetof(net_nat_rule_t, nat_addr) == 16 &&
               __builtin_offsetof(net_nat_rule_t, nat_port) == 20 &&
               __builtin_offsetof(net_nat_rule_t, matched) == 24 &&
               __builtin_offsetof(net_nat_rule_t, bytes) == 32 &&
               sizeof(net_nat_rule_t) == 40,
               "net_nat_rule_t layout changed: update NatRule in netfilter_nat.rs");
_Static_assert(sizeof(net_conntrack_stats_t) == 48,
               "net_conntrack_stats_t layout changed: update ConntrackStats");

#define NET_CONNTRACK_MAX NET_PROFILE_CONNTRACK_ENTRIES
#define NET_CONNTRACK_BUCKETS NET_PROFILE_CONNTRACK_BUCKETS

/*
 * The profile comment in net_profile.h budgets "~64 bytes" per entry.  Pin it,
 * so a future field that blows the budget is a build error rather than a
 * silently larger .bss on a target that was sized against the old number.
 */
_Static_assert(sizeof(net_conntrack_entry_t) <= NET_PROFILE_CONNTRACK_ENTRY_BYTES,
               "conntrack entry exceeds the per-tier budget in net_profile.h");
_Static_assert(NET_CONNTRACK_MAX < NET_CONNTRACK_NONE,
               "the chain sentinel must be outside the table index range");
_Static_assert((NET_CONNTRACK_MAX % NET_CONNTRACK_BUCKETS) == 0,
               "bucket count must divide the table size for the modulus hash");

static net_conntrack_entry_t g_ct[NET_CONNTRACK_MAX];
static uint16_t
    g_ct_head[NET_CONNTRACK_BUCKETS]; /* NET_CONNTRACK_NONE = empty */
static uint16_t g_ct_rhead[NET_CONNTRACK_BUCKETS]; /* reply-direction chain */
static uint32_t g_ct_used;
static uint32_t g_ct_sweep; /* sweeper resume point */
static int32_t g_ct_enabled = 1;

/*
 * Bounded NAT storage; Rust transacts through the guarded view and the C
 * renderer copies values under the same CT lock.
 */
static net_nat_rule_t g_nat[NETFILTER_MAX_NAT_RULES];
static uint32_t g_nat_count;

#if CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_EMBEDDED
/*
 * The filter tables are unconditional .bss that a target pays for on every
 * boot whether or not netfilter is ever loaded, so on the tier whose whole
 * point is a bounded footprint they belong in the budget.  Checked here because
 * this is the only place both arrays exist.  32 entries x 56 B + 16 rules x
 * 80 B = 3072 B measured, against a 3328 B ceiling; the two bucket-head arrays
 * and the counters above are a further 72 B and are not counted, which is the
 * direction that lets the assert hold rather than the direction that could
 * hide a regression.
 */
_Static_assert(sizeof(g_ct) + sizeof(g_nat) <= NET_PROFILE_FILTER_BUDGET,
               "the embedded profile's conntrack and NAT tables exceed the "
               "filter term of its own memory budget");
#endif

static net_conntrack_stats_t g_ct_lifetime;
static uint32_t g_ct_sweeps;

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

/* Millisecond overrides; zero selects the compiled protocol default. State
 * timestamps are wrapping milliseconds, never truncated architecture ticks. */
static uint32_t g_to_ms[3];

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

    net_nat_rule_t rules[NETFILTER_MAX_NAT_RULES];
    uint64_t ct_flags = a20_netfilter_ct_lock();
    unsigned rule_count = g_nat_count;
    int enabled = g_ct_enabled;
    memcpy(rules, g_nat, sizeof(rules));
    a20_netfilter_ct_unlock(ct_flags);
    NETFILTER_EMIT("conntrack: %s\n", enabled ? "on" : "off");
    NETFILTER_EMIT("ct_capacity: %u\n", (unsigned)NET_CONNTRACK_MAX);
    NETFILTER_EMIT("ct_tracked: %llu\n", (unsigned long long)cs.tracked);
    NETFILTER_EMIT("ct_created: %llu\n", (unsigned long long)cs.created);
    NETFILTER_EMIT("ct_evicted: %llu\n", (unsigned long long)cs.evicted);
    NETFILTER_EMIT("ct_timeout: %llu\n", (unsigned long long)cs.timeout);
    NETFILTER_EMIT("ct_lru_victim: %d\n", netfilter_conntrack_lru_victim());
    NETFILTER_EMIT("ct_sweeps: %u\n", (unsigned)netfilter_conntrack_sweeps());
    NETFILTER_EMIT("ct_packets: %llu\n", (unsigned long long)cs.packets);
    NETFILTER_EMIT("nat_rules: %u\n", rule_count);

    for (unsigned i = 0; i < rule_count && off < bufsz; i++) {
        const net_nat_rule_t *r = &rules[i];
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
        if (e->proto == NETFILTER_PROTO_ICMP) {
            /*
             * Printed without the ":port" fields because there are none.  The
             * two 16-bit tuple fields hold the type (normalised to the request
             * value) and the identifier, and rendering them in the TCP/UDP
             * shape would read as "this flow goes to port 1234" -- which is
             * exactly the reading that sends someone looking for a port that
             * does not exist.
             */
            NETFILTER_EMIT("ct %d: %u.%u.%u.%u -> %u.%u.%u.%u proto=1 "
                           "icmp=echo id=%u state=%s nat=%s packets=%llu\n",
                           i, (e->src_addr >> 24) & 0xff,
                           (e->src_addr >> 16) & 0xff, (e->src_addr >> 8) & 0xff,
                           e->src_addr & 0xff, (e->dst_addr >> 24) & 0xff,
                           (e->dst_addr >> 16) & 0xff, (e->dst_addr >> 8) & 0xff,
                           e->dst_addr & 0xff, e->dst_port,
                           e->state == NET_CONNTRACK_ESTABLISHED ? "established"
                                                                : "new",
                           netfilter_nat_name(e->nat),
                           (unsigned long long)e->packets);
            continue;
        }
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
    if (cs.tracked > (unsigned)n)
        NETFILTER_EMIT("ct ... %u more\n", (unsigned)cs.tracked - (unsigned)n);
}

#undef NETFILTER_EMIT

/*
 * Rust state machine.
 *
 * What is left here is the storage and the primitives: the two static tables
 * and their counters, the guarded view that hands them to Rust, and the
 * frame-rewrite entry points.  The translations, the chains, the LRU victim,
 * the idle sweep and every NAT decision are in kernel/net/filter/netfilter_nat.rs.
 */

static net_ct_view_t g_ct_view = {
    .entries = g_ct,
    .max = NET_CONNTRACK_MAX,
    .buckets = NET_CONNTRACK_BUCKETS,
    .head = g_ct_head,
    .rhead = g_ct_rhead,
    .used = (uint32_t *)&g_ct_used,
    .sweep = (uint32_t *)&g_ct_sweep,
    .enabled = (int32_t *)&g_ct_enabled,
    .lifetime = &g_ct_lifetime,
    .dir_stats = { &g_ct_dir_stats[0].stats, &g_ct_dir_stats[1].stats },
    .nat = g_nat,
    .nat_max = NETFILTER_MAX_NAT_RULES,
    .nat_count = (uint32_t *)&g_nat_count,
    .to_ms = g_to_ms,
    .sweeps = (uint32_t *)&g_ct_sweeps,
};

static const net_ct_view_t *net_ct_view(void)
{
    return &g_ct_view;
}

/* The filter half's hook calls this for an accepted packet: conntrack and NAT
 * run after the verdict, never before it. */
int a20_netfilter_ct_process_tail(uint8_t *frame, size_t len, int net_idx,
                                   uint8_t dir, const netfilter_frame_t *pkt)
{
    return a20_netfilter_ct_process(net_ct_view(), frame, len, net_idx, dir, pkt);
}

uint32_t a20_netfilter_now_ms(void)
{
    uint64_t ticks = timer_get_ticks(), hz = TICKS_PER_SEC;
    /* Divide before narrowing. Splitting seconds/remainder also avoids a
     * ticks*1000 overflow on long-running high-frequency platforms. */
    return (uint32_t)((ticks / hz) * 1000 + (ticks % hz) * 1000 / hz);
}

void a20_netfilter_rewrite_set_addr(uint8_t *frame, size_t len,
                                    const netfilter_frame_t *pkt,
                                    uint32_t new_src, uint32_t new_dst)
{
    netfilter_set_addr(frame, len, pkt, new_src, new_dst);
}

void a20_netfilter_rewrite_set_port(uint8_t *frame, size_t len,
                                    const netfilter_frame_t *pkt, int is_src,
                                    uint16_t new_port)
{
    netfilter_set_port(frame, len, pkt, is_src, new_port);
}

uint32_t a20_netfilter_masq_addr(int net_idx)
{
    return netfilter_masq_addr(net_idx);
}

void netfilter_conntrack_init(void)
{
    a20_netfilter_ct_init(net_ct_view());
    netfilter_busy_force(0);
}

void netfilter_conntrack_set_timeouts(unsigned tcp_new_ms, unsigned tcp_est_ms,
                                      unsigned udp_ms)
{
    a20_netfilter_ct_set_timeouts(net_ct_view(), tcp_new_ms, tcp_est_ms, udp_ms);
}

int netfilter_conntrack_count(void)
{
    return a20_netfilter_ct_count(net_ct_view());
}

unsigned netfilter_conntrack_sweeps(void)
{
    return a20_netfilter_ct_sweeps(net_ct_view());
}

int netfilter_conntrack_lru_victim(void)
{
    return a20_netfilter_ct_lru_victim(net_ct_view());
}

int netfilter_conntrack_inject(uint32_t src, uint32_t dst, uint16_t sport,
                               uint16_t dport, uint8_t proto, uint8_t state)
{
    int r = a20_netfilter_ct_inject(net_ct_view(), src, dst, sport, dport,
                                    proto, state);
    if (r == 0)
        netfilter_busy_refresh();
    return r;
}

int netfilter_conntrack_snapshot(net_conntrack_entry_t *out, unsigned max)
{
    return a20_netfilter_ct_snapshot(net_ct_view(), out, max);
}

void netfilter_conntrack_flush(void)
{
    a20_netfilter_ct_flush(net_ct_view());
    netfilter_busy_refresh();
}

void netfilter_conntrack_get_stats(net_conntrack_stats_t *out)
{
    a20_netfilter_ct_get_stats(net_ct_view(), out);
}

void netfilter_conntrack_set_enabled(int on)
{
    a20_netfilter_ct_set_enabled(net_ct_view(), on);
    /* cton is also the explicit "track even with no rules" request; ctoff
     * stops new entries but leaves existing NAT bindings in force. */
    netfilter_busy_force(on);
}

int netfilter_conntrack_enabled(void)
{
    return a20_netfilter_ct_enabled(net_ct_view());
}

unsigned netfilter_conntrack_expire(unsigned max_scan)
{
    unsigned removed = a20_netfilter_ct_expire(net_ct_view(), max_scan);
    if (removed)
        netfilter_busy_refresh();
    return removed;
}

int netfilter_conntrack_process(uint8_t *frame, size_t len, int net_idx,
                                 netfilter_dir_t dir,
                                 const netfilter_frame_t *pkt)
{
    return a20_netfilter_ct_process(net_ct_view(), frame, len, net_idx, (uint8_t)dir,
                             pkt);
}

int netfilter_nat_rule_count(void)
{
    return a20_netfilter_nat_count(net_ct_view());
}

int netfilter_nat_add_rule(const net_nat_rule_t *rule)
{
    if (!rule)
        return -EINVAL;
    /* Serialize configuration writers, then publish under the CT guard. */
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    int r = a20_netfilter_nat_add(net_ct_view(), rule);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    if (r == 0)
        netfilter_busy_refresh();
    return r;
}

int netfilter_nat_del_rule(unsigned index)
{
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    int r = a20_netfilter_nat_del(net_ct_view(), index);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    if (r == 0)
        netfilter_busy_refresh();
    return r;
}

void netfilter_nat_reset(void)
{
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    a20_netfilter_nat_reset(net_ct_view());
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    netfilter_busy_refresh();
}

int netfilter_nat_parse_rule(const char *line, size_t len,
                             net_nat_rule_t *out)
{
    return a20_netfilter_nat_parse((const uint8_t *)line, len, out);
}
