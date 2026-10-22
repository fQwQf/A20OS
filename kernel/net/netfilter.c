/*
 * IPv4 packet filter -- rule evaluation, rule table and counters.
 *
 * See kernel/include/net/netfilter.h for the locking contract: the hooks run
 * under g_lwip_lock, so nothing here allocates, blocks, or calls into lwIP.
 * Evaluation is a linear scan over a small fixed table of already-parsed
 * rules, which is bounded and allocation-free.
 *
 * The table is mutated only through netfilter_add_rule/del_rule/reset, which
 * take g_netfilter_lock.  The hooks cannot take that lock -- it is a different
 * lock from the one they already hold, so acquiring it here would both add
 * per-packet serialisation and constrain the lock order -- and they used to
 * read the table with no protection at all.  That was a live race, not a
 * theoretical one: del_rule() shifts the whole array down while a reader walks
 * it, and in add_rule() the store to g_rules[n] is not ordered against the
 * g_rule_count++ that publishes it, so a reader could act on a slot it believes
 * is live but has not been written yet.
 *
 * g_rule_seq is the generation counter, and it is what makes those safe.
 * Writers bracket their mutation with an odd then even value; readers sample
 * it around the scan and retry when it moved or was odd.  Readers stay
 * wait-free, which taking a lock under the global lwIP lock would not be.
 *
 * A packet meets three things here, in this order: the rule table (this file),
 * then conntrack and NAT (kernel/net/netfilter_nat.c).  The order is
 * deliberate -- a dropped packet must not create flow state or consume a NAT
 * binding -- and it is why the Ethernet/IPv4/L4 walk lives in
 * netfilter_parse_frame() below and is handed to the NAT half rather than
 * repeated there.
 */

#include "net/netfilter.h"
#include "net/netfilter_internal.h"
#include "net/lwip_stack.h"
#include "core/lock.h"
#include "core/seqlock.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/errno.h"

#include "net/netfilter_filter_state.h"

/*
 * The Rust mirror of these three structs is field for field, so every offset is
 * pinned here: a change to net/netfilter.h that moves a field must fail this
 * build rather than corrupt the matcher on the other side.
 */
_Static_assert(__builtin_offsetof(netfilter_rule_t, src_port) == 4 &&
               __builtin_offsetof(netfilter_rule_t, dst_port) == 6 &&
               __builtin_offsetof(netfilter_rule_t, src_addr) == 8 &&
               __builtin_offsetof(netfilter_rule_t, dst_addr) == 12 &&
               __builtin_offsetof(netfilter_rule_t, matched) == 16 &&
               __builtin_offsetof(netfilter_rule_t, bytes) == 24 &&
               sizeof(netfilter_rule_t) == 32,
               "netfilter_rule_t layout changed: update FilterRule");
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
               "netfilter_frame_t layout changed: update Frame");
_Static_assert(sizeof(netfilter_stats_t) == 48,
               "netfilter_stats_t layout changed: update FilterStats");

static netfilter_rule_t g_rules[NETFILTER_MAX_RULES];
static unsigned g_rule_count;

/*
 * Seqlock over the rule table.  Even means the table is stable, odd means a
 * writer is inside it.  Writers hold g_netfilter_lock, so they are already
 * serialised against each other; this exists for the lock-free readers on the
 * packet path, which run under a different lock entirely.
 */
static seqlock_t g_rule_seq = SEQLOCK_INIT;

/* Shared with netfilter_nat.c; see netfilter_internal.h for why the NAT table
 * uses the same writer lock rather than one of its own. */
spinlock_t g_netfilter_lock;

static void netfilter_rules_begin(void)
{
    seqlock_write_begin(&g_rule_seq);
}

static void netfilter_rules_end(void)
{
    seqlock_write_end(&g_rule_seq);
}

/*
 * Counters, one cache-line-separated bank per direction.
 *
 * Every evaluated packet updates a counter in each direction, so a single
 * netfilter_stats_t puts the receive path's increment and the transmit path's
 * increment on the same line and makes every CPU contend for it.  Only the
 * three counters belonging to a bank's own direction are used, which is why
 * each bank is a whole netfilter_stats_t rather than three bare fields.
 */
#define NETFILTER_STATS_STRIDE 128

typedef struct {
    netfilter_stats_t stats;
    uint8_t pad[NETFILTER_STATS_STRIDE - sizeof(netfilter_stats_t)];
} netfilter_stats_bank_t;

static netfilter_stats_bank_t g_stats[2];


int netfilter_ipv4_from_str(const char *s, size_t len, uint32_t *out)
{
    uint32_t parts[4] = { 0, 0, 0, 0 };
    size_t i = 0, p = 0;
    while (i < len && p < 4) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        uint32_t v = 0;
        size_t digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (uint32_t)(s[i] - '0');
            i++;
            if (++digits > 3)
                return 0;
        }
        parts[p++] = v;
        if (i < len) {
            if (s[i] != '.')
                return 0;
            i++;
        }
    }
    if (p != 4)
        return 0;
    for (p = 0; p < 4; p++) {
        if (parts[p] > 255)
            return 0;
    }
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

void netfilter_ipv4_to_str(uint32_t addr, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%u.%u.%u.%u", (addr >> 24) & 0xff,
             (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff);
}

static const char *proto_name(uint8_t proto)
{
    switch (proto) {
    case NETFILTER_PROTO_ICMP: return "icmp";
    case NETFILTER_PROTO_TCP:  return "tcp";
    case NETFILTER_PROTO_UDP:  return "udp";
    default:                   return "any";
    }
}



/*
 * Extract src/dst address, protocol, ports and header offsets from an
 * Ethernet+IPv4 frame.  Returns 0 when the frame is not IPv4 or is truncated;
 * VLAN tags are skipped.  Ports are only meaningful for TCP/UDP and are set to
 * NETFILTER_NO_PORT otherwise.
 *
 * The offsets are part of the result rather than something the NAT rewriter
 * re-derives: a rewriter working from its own arithmetic could disagree with
 * the matcher about where the L4 header starts, and the two would then rewrite
 * different bytes of the same packet.
 */

/*
 * True when the table holds at least one rule.  Sampled under the seqlock so
 * the fast path cannot report an empty table for a rule that has just been
 * installed: a writer publishes the count inside netfilter_rules_begin/end,
 * so a table that will not settle is treated as populated and the full
 * evaluation runs.  That direction costs a rule scan, which is the right way
 * to be wrong about a filter.
 *
 * The header parse this used to skip is no longer skippable -- conntrack needs
 * the 5-tuple on every TCP/UDP packet -- so the fast path now saves only the
 * scan.  What it no longer saves is the counters, which are now maintained for
 * every packet; see netfilter_eval.
 */

/*
 * The 4096-byte buffer procfs hands the renderer is not enough for the filter
 * table, the NAT table and the conntrack table at once, so the filter section
 * is written first and netfilter_nat_format() appends the rest under the
 * remaining budget.  Every cursor here is clamped: snprintf reports the length
 * it *would* have written, and an unclamped cursor would make every later
 * bufsz - off underflow into a huge size.
 */
static size_t netfilter_append(char *buf, size_t bufsz, size_t off,
                               const char *s)
{
    while (*s && off < bufsz)
        buf[off++] = *s++;
    return off;
}

void netfilter_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return;
    char line[256];
    size_t off = 0;

    netfilter_stats_t s;
    netfilter_get_stats(&s);
    snprintf(line, sizeof(line),
        "policy: accept\n"
        "in_packets: %llu\n"
        "out_packets: %llu\n"
        "in_dropped: %llu\n"
        "out_dropped: %llu\n"
        "in_accepted: %llu\n"
        "out_accepted: %llu\n"
        "rules: %u\n",
        (unsigned long long)s.in_packets, (unsigned long long)s.out_packets,
        (unsigned long long)s.in_dropped, (unsigned long long)s.out_dropped,
        (unsigned long long)s.in_accepted, (unsigned long long)s.out_accepted,
        g_rule_count);
    off = netfilter_append(buf, bufsz, off, line);
    if (off < bufsz && g_rule_count) {
        off = netfilter_append(buf, bufsz, off, "(filter rules)\n");
        for (unsigned i = 0; i < g_rule_count && off < bufsz; i++) {
            const netfilter_rule_t *r = &g_rules[i];
            char src[16] = "any", dst[16] = "any";
            if (r->src_addr != NETFILTER_NO_ADDR)
                netfilter_ipv4_to_str(r->src_addr, src, sizeof(src));
            if (r->dst_addr != NETFILTER_NO_ADDR)
                netfilter_ipv4_to_str(r->dst_addr, dst, sizeof(dst));
            char sport[8] = "any", dport[8] = "any";
            if (r->src_port != NETFILTER_NO_PORT)
                snprintf(sport, sizeof(sport), "%u", r->src_port);
            if (r->dst_port != NETFILTER_NO_PORT)
                snprintf(dport, sizeof(dport), "%u", r->dst_port);
            snprintf(line, sizeof(line),
                "rule %u: %s proto=%s src=%s dst=%s sport=%s dport=%s "
                "action=%s matched=%llu bytes=%llu\n",
                i, r->dir == NETFILTER_DIR_IN ? "in" : "out",
                proto_name(r->proto), src, dst, sport, dport,
                r->action == NETFILTER_DROP ? "drop" : "accept",
                (unsigned long long)__atomic_load_n(&r->matched,
                                                    __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&r->bytes,
                                                    __ATOMIC_RELAXED));
            off = netfilter_append(buf, bufsz, off, line);
        }
    }

    /*
     * The conntrack and NAT sections are snapshots of state the packet path
     * mutates in place, so they are taken under g_lwip_lock -- the lock the
     * packet path holds.  The filter section above is not: its table is read
     * through a seqlock precisely so no lock is needed.  Safe from here
     * because procfs rendering runs from a read(), never under g_lwip_lock.
     */
    uint64_t flags = a20_lwip_lock();
    netfilter_nat_format(buf, bufsz, off);
    a20_lwip_unlock(flags);
}


/*
 * Rust filter (NETFILTER_FILTER_IMPL=rust).
 *
 * What is left here is the storage, the writer lock, the rule seqlock, the
 * dotted-quad conversions and the renderer.  Which rule matches, what the
 * parser accepts, the frame walk and the hook ordering are the Rust state
 * machine's.
 */

static net_filter_view_t g_filter_view = {
    .rules = g_rules,
    .max = NETFILTER_MAX_RULES,
    .count = (uint32_t *)&g_rule_count,
    .stats_in = &g_stats[0].stats,
    .stats_out = &g_stats[1].stats,
};

static const net_filter_view_t *net_filter_view(void)
{
    return &g_filter_view;
}

uint32_t a20_netfilter_rule_seq_read_begin(void)
{
    return seqlock_read_begin(&g_rule_seq);
}

int a20_netfilter_rule_seq_read_retry(uint32_t start)
{
    return seqlock_read_retry(&g_rule_seq, start) ? 1 : 0;
}

void a20_netfilter_rule_seq_write_begin(void)
{
    netfilter_rules_begin();
}

void a20_netfilter_rule_seq_write_end(void)
{
    netfilter_rules_end();
}

int a20_netfilter_ipv4_from_str(const uint8_t *s, size_t len, uint32_t *out)
{
    return netfilter_ipv4_from_str((const char *)s, len, out);
}

int netfilter_parse_rule(const char *line, size_t len, netfilter_rule_t *out)
{
    return a20_netfilter_parse_rule((const uint8_t *)line, len, out);
}

int netfilter_rule_count(void)
{
    return a20_netfilter_rule_count(net_filter_view());
}

int netfilter_add_rule(const netfilter_rule_t *rule)
{
    if (!rule)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    int r = a20_netfilter_rule_add(net_filter_view(), rule);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return r;
}

int netfilter_del_rule(unsigned index)
{
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    int r = a20_netfilter_rule_del(net_filter_view(), index);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return r;
}

void netfilter_reset(void)
{
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    a20_netfilter_rule_reset(net_filter_view());
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
}

int netfilter_get_rule(unsigned index, netfilter_rule_t *out)
{
    if (!out)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    int r = a20_netfilter_rule_get(net_filter_view(), index, out);
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return r;
}

void netfilter_get_stats(netfilter_stats_t *out)
{
    a20_netfilter_rule_stats(net_filter_view(), out);
}

int netfilter_parse_frame(const uint8_t *f, size_t len, netfilter_frame_t *out)
{
    return a20_netfilter_parse_frame(f, len, out);
}

static netfilter_action_t netfilter_process(uint8_t *frame, size_t len,
                                            int net_idx, netfilter_dir_t dir)
{
    return (netfilter_action_t)a20_netfilter_process(net_filter_view(), frame,
                                                     len, net_idx, (uint8_t)dir);
}

netfilter_action_t netfilter_input(uint8_t *frame, size_t len, int net_idx)
{
    return netfilter_process(frame, len, net_idx, NETFILTER_DIR_IN);
}

netfilter_action_t netfilter_output(uint8_t *frame, size_t len, int net_idx)
{
    return netfilter_process(frame, len, net_idx, NETFILTER_DIR_OUT);
}

