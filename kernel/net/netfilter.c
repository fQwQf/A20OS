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
 */

#include "net/netfilter.h"
#include "core/lock.h"
#include "core/seqlock.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/errno.h"

static netfilter_rule_t g_rules[NETFILTER_MAX_RULES];
static unsigned g_rule_count;

/*
 * Seqlock over the rule table.  Even means the table is stable, odd means a
 * writer is inside it.  Writers hold g_netfilter_lock, so they are already
 * serialised against each other; this exists for the lock-free readers on the
 * packet path, which run under a different lock entirely.
 */
static seqlock_t g_rule_seq = SEQLOCK_INIT;

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

static netfilter_stats_t *netfilter_stats_dir(netfilter_dir_t dir)
{
    return &g_stats[dir == NETFILTER_DIR_IN ? 0 : 1].stats;
}

static spinlock_t g_netfilter_lock;

static uint32_t ipv4_from_str(const char *s, size_t len, uint32_t *out)
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

static void ipv4_to_str(uint32_t addr, char *buf, size_t bufsz)
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

int netfilter_parse_rule(const char *line, size_t len, netfilter_rule_t *out)
{
    if (!line || !out)
        return -EINVAL;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;

    memset(out, 0, sizeof(*out));
    out->proto = NETFILTER_PROTO_ANY;
    out->action = NETFILTER_ACCEPT;
    out->src_addr = NETFILTER_NO_ADDR;
    out->dst_addr = NETFILTER_NO_ADDR;
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
        size_t used = 0, klen = 0;
        while (i + klen < len && line[i + klen] != '=')
            klen++;
        if (i + klen >= len)
            break;
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
            } else if (vlen >= 1 && vlen <= 3 && val[0] >= '0' && val[0] <= '9') {
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
            } else {
                uint32_t a;
                if (!ipv4_from_str(val, vlen, &a))
                    return -EINVAL;
                out->src_addr = a;
            }
        } else if (klen == 3 && strncmp(key, "dst", 3) == 0) {
            if (vlen == 3 && strncmp(val, "any", 3) == 0) {
                out->dst_addr = NETFILTER_NO_ADDR;
            } else {
                uint32_t a;
                if (!ipv4_from_str(val, vlen, &a))
                    return -EINVAL;
                out->dst_addr = a;
            }
        } else if (klen == 5 && strncmp(key, "sport", 5) == 0) {
            if (vlen == 3 && strncmp(val, "any", 3) == 0) {
                out->src_port = NETFILTER_NO_PORT;
            } else {
                uint32_t p = 0;
                for (size_t k = 0; k < vlen; k++) {
                    if (val[k] < '0' || val[k] > '9')
                        return -EINVAL;
                    p = p * 10 + (uint32_t)(val[k] - '0');
                    if (p > NETFILTER_NO_PORT)
                        return -EINVAL;
                }
                out->src_port = (uint16_t)p;
            }
        } else if (klen == 5 && strncmp(key, "dport", 5) == 0) {
            if (vlen == 3 && strncmp(val, "any", 3) == 0) {
                out->dst_port = NETFILTER_NO_PORT;
            } else {
                uint32_t p = 0;
                for (size_t k = 0; k < vlen; k++) {
                    if (val[k] < '0' || val[k] > '9')
                        return -EINVAL;
                    p = p * 10 + (uint32_t)(val[k] - '0');
                    if (p > NETFILTER_NO_PORT)
                        return -EINVAL;
                }
                out->dst_port = (uint16_t)p;
            }
        } else if (klen == 6 && strncmp(key, "action", 6) == 0) {
            if (vlen == 6 && strncmp(val, "accept", 6) == 0) {
                out->action = NETFILTER_ACCEPT;
            } else if (vlen == 4 && strncmp(val, "drop", 4) == 0) {
                out->action = NETFILTER_DROP;
            } else {
                return -EINVAL;
            }
        } else {
            return -EINVAL;
        }

        (void)used;
        i += klen + 1 + vlen;
        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            i++;
    }
    return 0;
}

int netfilter_rule_count(void)
{
    /* Acquire: this is a lock-free reader, so it must not observe a
     * g_rule_count that has been published ahead of the slot it describes. */
    return (int)__atomic_load_n(&g_rule_count, __ATOMIC_ACQUIRE);
}

int netfilter_add_rule(const netfilter_rule_t *rule)
{
    if (!rule)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    if (g_rule_count >= NETFILTER_MAX_RULES) {
        spin_unlock_irqrestore(&g_netfilter_lock, flags);
        return -ENOSPC;
    }
    netfilter_rules_begin();
    g_rules[g_rule_count] = *rule;
    g_rule_count++;
    netfilter_rules_end();
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return (int)(g_rule_count - 1);
}

int netfilter_del_rule(unsigned index)
{
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    if (index >= g_rule_count) {
        spin_unlock_irqrestore(&g_netfilter_lock, flags);
        return -EINVAL;
    }
    netfilter_rules_begin();
    for (unsigned i = index; i + 1 < g_rule_count; i++)
        g_rules[i] = g_rules[i + 1];
    g_rule_count--;
    memset(&g_rules[g_rule_count], 0, sizeof(g_rules[0]));
    netfilter_rules_end();
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return 0;
}

void netfilter_reset(void)
{
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    netfilter_rules_begin();
    memset(g_rules, 0, sizeof(g_rules));
    g_rule_count = 0;
    netfilter_rules_end();
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
}

int netfilter_get_rule(unsigned index, netfilter_rule_t *out)
{
    if (!out)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_netfilter_lock);
    if (index >= g_rule_count) {
        spin_unlock_irqrestore(&g_netfilter_lock, flags);
        return -EINVAL;
    }
    *out = g_rules[index];
    spin_unlock_irqrestore(&g_netfilter_lock, flags);
    return 0;
}

void netfilter_get_stats(netfilter_stats_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    for (unsigned d = 0; d < 2; d++) {
        const netfilter_stats_t *s = &g_stats[d].stats;
        out->in_packets += __atomic_load_n(&s->in_packets, __ATOMIC_RELAXED);
        out->in_dropped += __atomic_load_n(&s->in_dropped, __ATOMIC_RELAXED);
        out->in_accepted += __atomic_load_n(&s->in_accepted, __ATOMIC_RELAXED);
        out->out_packets += __atomic_load_n(&s->out_packets, __ATOMIC_RELAXED);
        out->out_dropped += __atomic_load_n(&s->out_dropped, __ATOMIC_RELAXED);
        out->out_accepted += __atomic_load_n(&s->out_accepted, __ATOMIC_RELAXED);
    }
}

/*
 * Extract src/dst address, protocol and ports from an Ethernet+IPv4 frame.
 * Returns 0 when the frame is not IPv4 or is truncated; VLAN tags are
 * skipped.  Ports are only meaningful for TCP/UDP and are set to
 * NETFILTER_NO_PORT otherwise.
 */
static int netfilter_parse_frame(const uint8_t *f, size_t len, uint8_t *proto,
                                 uint32_t *src, uint32_t *dst,
                                 uint16_t *sport, uint16_t *dport)
{
    size_t off = 14;
    if (len < off)
        return 0;
    uint16_t ethertype = (uint16_t)((f[12] << 8) | f[13]);
    while (ethertype == 0x8100 || ethertype == 0x88a8) {
        if (len < off + 4)
            return 0;
        ethertype = (uint16_t)((f[off + 2] << 8) | f[off + 3]);
        off += 4;
    }
    if (ethertype != 0x0800)
        return 0;
    if (len < off + 20)
        return 0;
    const uint8_t *ip = f + off;
    if (((ip[0] >> 4) & 0xf) != 4)
        return 0;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    if (ihl < 20 || len < off + ihl)
        return 0;
    /* Fragmented non-first fragments carry no usable L4 header. */
    uint16_t frag = (uint16_t)(((ip[6] << 8) | ip[7]) & 0x1fff);
    *proto = ip[9];
    *src = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) |
           ((uint32_t)ip[14] << 8) | ip[15];
    *dst = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
           ((uint32_t)ip[18] << 8) | ip[19];
    *sport = NETFILTER_NO_PORT;
    *dport = NETFILTER_NO_PORT;
    if (frag != 0)
        return 1;
    if ((*proto == NETFILTER_PROTO_TCP || *proto == NETFILTER_PROTO_UDP) &&
        len >= off + ihl + 4) {
        const uint8_t *l4 = ip + ihl;
        *sport = (uint16_t)((l4[0] << 8) | l4[1]);
        *dport = (uint16_t)((l4[2] << 8) | l4[3]);
    }
    return 1;
}

/*
 * True when the table holds at least one rule.  Sampled under the seqlock so
 * the fast path cannot report an empty table for a rule that has just been
 * installed: a writer publishes the count inside netfilter_rules_begin/end,
 * so a table that will not settle is treated as populated and the full
 * evaluation runs.  That direction costs a parse, which is the right way to
 * be wrong about a filter.
 */
static int netfilter_table_populated(void)
{
    for (unsigned attempt = 0; attempt < SEQLOCK_READ_ATTEMPTS; attempt++) {
        unsigned seq0 = seqlock_read_begin(&g_rule_seq);
        if (seq0 == 0u)
            continue;               /* writer inside the table; wait for it */
        unsigned n = __atomic_load_n(&g_rule_count, __ATOMIC_RELAXED);
        if (!seqlock_read_retry(&g_rule_seq, seq0))
            return n != 0;
    }
    return 1;
}

static netfilter_action_t netfilter_eval(const void *frame, size_t len,
                                         netfilter_dir_t dir)
{
    if (!frame || len < 14)
        return NETFILTER_ACCEPT;

    /*
     * With an empty table the verdict is ACCEPT whatever the frame holds, so
     * there is nothing to parse and nothing to count: an unconfigured system
     * would otherwise walk Ethernet, VLAN, IPv4 and L4 headers and bump two
     * shared counters on every packet in both directions to reach the same
     * answer.  Once a rule exists the parse and the counters run as before.
     */
    if (!netfilter_table_populated())
        return NETFILTER_ACCEPT;

    uint8_t proto;
    uint32_t src, dst;
    uint16_t sport, dport;
    if (!netfilter_parse_frame((const uint8_t *)frame, len, &proto, &src, &dst,
                               &sport, &dport))
        return NETFILTER_ACCEPT;

    netfilter_stats_t *st = netfilter_stats_dir(dir);
    if (dir == NETFILTER_DIR_IN)
        __atomic_fetch_add(&st->in_packets, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&st->out_packets, 1, __ATOMIC_RELAXED);

    /*
     * Scan under the seqlock.  The table can be mutated concurrently by
     * netfilter_add_rule/del_rule/reset under g_netfilter_lock, which is not
     * the lock this path holds, so the scan is bracketed by a generation
     * sample and retried if it moved.  A retry leaves `hit` unset, which
     * yields the same default-accept answer an unconfigured filter gives.
     *
     * The matched rule's own counters are bumped inside the bracket.  Bumping
     * them after it would risk charging a packet to whichever rule slid into
     * that index; on a retry they are simply not charged, which understates a
     * counter rather than misattributing it.
     */
    int hit = -1;
    netfilter_action_t action = NETFILTER_ACCEPT;
    for (unsigned attempt = 0; attempt < SEQLOCK_READ_ATTEMPTS; attempt++) {
        unsigned seq0 = seqlock_read_begin(&g_rule_seq);
        if (seq0 == 0u)
            continue;               /* writer inside the table; wait for it */
        unsigned n = __atomic_load_n(&g_rule_count, __ATOMIC_RELAXED);
        int found = -1;
        netfilter_action_t found_action = NETFILTER_ACCEPT;
        for (unsigned i = 0; i < n; i++) {
            netfilter_rule_t *r = &g_rules[i];
            if (r->dir != dir)
                continue;
            if (r->proto != NETFILTER_PROTO_ANY && r->proto != proto)
                continue;
            if (r->src_addr != NETFILTER_NO_ADDR && r->src_addr != src)
                continue;
            if (r->dst_addr != NETFILTER_NO_ADDR && r->dst_addr != dst)
                continue;
            if (r->src_port != NETFILTER_NO_PORT && r->src_port != sport)
                continue;
            if (r->dst_port != NETFILTER_NO_PORT && r->dst_port != dport)
                continue;
            found = (int)i;
            found_action = (netfilter_action_t)r->action;
            break;
        }
        if (seqlock_read_retry(&g_rule_seq, seq0))
            continue;               /* moved under us; rescan */
        hit = found;
        action = found_action;
        if (hit >= 0) {
            netfilter_rule_t *r = &g_rules[hit];
            __atomic_fetch_add(&r->matched, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&r->bytes, len, __ATOMIC_RELAXED);
        }
        break;
    }

    /* No rule matched, the table never settled, or no rule applies: default
     * policy is accept, so an unconfigured or non-matching system behaves
     * exactly as it did before this existed. */
    if (hit >= 0 && action == NETFILTER_DROP) {
        if (dir == NETFILTER_DIR_IN)
            __atomic_fetch_add(&st->in_dropped, 1, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&st->out_dropped, 1, __ATOMIC_RELAXED);
        return NETFILTER_DROP;
    }
    if (dir == NETFILTER_DIR_IN)
        __atomic_fetch_add(&st->in_accepted, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&st->out_accepted, 1, __ATOMIC_RELAXED);
    return NETFILTER_ACCEPT;
}

netfilter_action_t netfilter_input(const void *frame, size_t len)
{
    return netfilter_eval(frame, len, NETFILTER_DIR_IN);
}

netfilter_action_t netfilter_output(const void *frame, size_t len)
{
    return netfilter_eval(frame, len, NETFILTER_DIR_OUT);
}

void netfilter_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return;
    size_t off = 0;

    netfilter_stats_t s;
    netfilter_get_stats(&s);
    off += (size_t)snprintf(buf + off, bufsz - off,
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
    if (off >= bufsz)
        return;

    for (unsigned i = 0; i < g_rule_count && off < bufsz; i++) {
        const netfilter_rule_t *r = &g_rules[i];
        char src[16] = "any", dst[16] = "any";
        if (r->src_addr != NETFILTER_NO_ADDR)
            ipv4_to_str(r->src_addr, src, sizeof(src));
        if (r->dst_addr != NETFILTER_NO_ADDR)
            ipv4_to_str(r->dst_addr, dst, sizeof(dst));
        char sport[8] = "any", dport[8] = "any";
        if (r->src_port != NETFILTER_NO_PORT)
            snprintf(sport, sizeof(sport), "%u", r->src_port);
        if (r->dst_port != NETFILTER_NO_PORT)
            snprintf(dport, sizeof(dport), "%u", r->dst_port);
        off += (size_t)snprintf(buf + off, bufsz - off,
            "rule %u: %s proto=%s src=%s dst=%s sport=%s dport=%s action=%s "
            "matched=%llu bytes=%llu\n",
            i, r->dir == NETFILTER_DIR_IN ? "in" : "out",
            proto_name(r->proto), src, dst, sport, dport,
            r->action == NETFILTER_DROP ? "drop" : "accept",
            (unsigned long long)__atomic_load_n(&r->matched, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&r->bytes, __ATOMIC_RELAXED));
    }
}
