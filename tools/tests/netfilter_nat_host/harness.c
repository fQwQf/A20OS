/*
 * Differential harness for the exact production kernel/net/netfilter_nat.c.
 *
 * Both selector runs compile this same translation unit and drive the same
 * scenarios through the production entry points, so the transcripts must be
 * byte-identical: the C reference under #else and the Rust state machine in
 * kernel/net/netfilter_nat.rs are compared on the real code, not on a
 * reimplementation of it.
 *
 * What the harness supplies is the primitive half only: a controllable clock
 * (so idle expiry and LRU are deterministic), the netif address a MASQUERADE
 * takes, the shared dotted-quad parser, libc, and the host lock shim.  The frame
 * builders are test code; the rewrite itself is the production
 * netfilter_rewrite.c, compiled in.
 */
#include "net/netfilter.h"
#include "net/netfilter_internal.h"
#include "net/netfilter_rewrite.h"
#include "net/net_profile.h"

#include "core/stdio.h" /* va_list without <stdarg.h> */
#include "/usr/include/stdio.h"
#include "/usr/include/stdlib.h"
#include "/usr/include/string.h"
#include <unistd.h>

/* netfilter.c owns the global writer lock the NAT entry points take; the
 * harness defines it so the production paths link unmodified. */
spinlock_t g_netfilter_lock = SPINLOCK_INIT;

/* ------------------------------------------------------------------ clock */

static uint32_t g_now;

uint64_t timer_get_ticks(void)
{
    return g_now;
}

/* The MASQUERADE source address: a fixed interface address so the transcript
 * does not depend on any real netif state. */
uint32_t a20_lwip_netif_ipv4(int net_idx)
{
    (void)net_idx;
    return 0x0a00020fu; /* 10.0.2.15 */
}

/* The shared dotted-quad parser lives in netfilter.c, which drags the filter
 * table and hooks in; the state machine under test only calls it from the NAT
 * rule parser, so a small faithful stand-in is enough here. */
int netfilter_ipv4_from_str(const char *s, size_t len, uint32_t *out)
{
    if (!s || !out || len == 0)
        return 0;
    uint32_t v = 0;
    unsigned parts = 0, cur = 0, digits = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = (i == len) ? '.' : s[i];
        if (c == '.') {
            if (!digits || parts == 4)
                return 0;
            v = (v << 8) | cur;
            parts++;
            cur = 0;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            cur = cur * 10 + (unsigned)(c - '0');
            if (cur > 255)
                return 0;
            digits++;
        } else {
            return 0;
        }
    }
    if (parts != 4)
        return 0;
    *out = v;
    return 1;
}

/* The filter half owns the C ABI for the shared parser; this harness does not
 * link netfilter.c, so it forwards to its own stub. */
int a20_netfilter_ipv4_from_str(const uint8_t *s, size_t len, uint32_t *out)
{
    return netfilter_ipv4_from_str((const char *)s, len, out);
}

void netfilter_ipv4_to_str(uint32_t addr, char *buf, size_t len)
{
    if (!buf || !len)
        return;
    snprintf(buf, len, "%u.%u.%u.%u", (unsigned)(addr >> 24) & 0xff,
             (unsigned)(addr >> 16) & 0xff, (unsigned)(addr >> 8) & 0xff,
             (unsigned)addr & 0xff);
}

/* Panic path for the Rust runtime archive. */
__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    (void)fmt;
    (void)!write(2, "HOST PANIC\n", 11);
    abort();
}

void rust_eh_personality(void) { }

/* ---------------------------------------------------------------- helpers */

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int g_fail;

#define NO_ADDR 0xFFFFFFFFu
#define NO_PORT 0xFFFFu

/* One Ethernet + IPv4 + TCP/UDP frame.  Returns the parsed view. */
static void build_frame(uint8_t *f, size_t *len, netfilter_frame_t *pkt,
                        uint8_t proto, uint32_t src, uint32_t dst,
                        uint16_t sport, uint16_t dport)
{
    memset(f, 0, 64);
    memset(pkt, 0, sizeof(*pkt));
    /* Ethernet */
    memset(f, 0x11, 6);
    memset(f + 6, 0x22, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    /* IPv4 */
    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    ip[2] = 0;
    ip[3] = 40;
    ip[8] = 64;
    ip[9] = proto;
    ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);  ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dst >> 24); ip[17] = (uint8_t)(dst >> 16);
    ip[18] = (uint8_t)(dst >> 8);  ip[19] = (uint8_t)dst;
    /* L4 */
    uint8_t *l4 = f + 34;
    l4[0] = (uint8_t)(sport >> 8); l4[1] = (uint8_t)sport;
    l4[2] = (uint8_t)(dport >> 8); l4[3] = (uint8_t)dport;
    *len = 54;

    pkt->src_addr = src;
    pkt->dst_addr = dst;
    pkt->src_port = sport;
    pkt->dst_port = dport;
    pkt->proto = proto;
    pkt->has_ports = 1;
    pkt->ip_off = 14;
    pkt->l4_off = 34;
}

static uint32_t frame_ip_src(const uint8_t *f)
{
    const uint8_t *ip = f + 14;
    return ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) |
           ((uint32_t)ip[14] << 8) | ip[15];
}

static uint32_t frame_ip_dst(const uint8_t *f)
{
    const uint8_t *ip = f + 14;
    return ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
           ((uint32_t)ip[18] << 8) | ip[19];
}

static uint16_t frame_sport(const uint8_t *f)
{
    return (uint16_t)(((uint16_t)f[34] << 8) | f[35]);
}

static uint16_t frame_dport(const uint8_t *f)
{
    return (uint16_t)(((uint16_t)f[36] << 8) | f[37]);
}

/* ---------------------------------------------------------------- scenarios */

static void scenario_tracking(void)
{
    uint8_t f[64];
    size_t len;
    netfilter_frame_t pkt;

    netfilter_conntrack_flush();
    netfilter_conntrack_set_enabled(1);
    say("track: count_after_flush=%d", netfilter_conntrack_count());

    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
                40000, 80);
    g_now = 1000;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("track: after_new count=%d", netfilter_conntrack_count());

    net_conntrack_stats_t st;
    netfilter_conntrack_get_stats(&st);
    say("track: stats tracked=%llu created=%llu evicted=%llu timeout=%llu "
        "packets=%llu bytes=%llu",
        (unsigned long long)st.tracked, (unsigned long long)st.created,
        (unsigned long long)st.evicted, (unsigned long long)st.timeout,
        (unsigned long long)st.packets, (unsigned long long)st.bytes);

    /* Reply arrives: same tuple reversed.  The entry becomes ESTABLISHED. */
    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x0a00020f, 0x0a000202,
                80, 40000);
    pkt.tcp_flags = 0x12; /* SYN|ACK */
    g_now = 1200;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    net_conntrack_entry_t snap[4];
    int n = netfilter_conntrack_snapshot(snap, 4);
    say("track: snapshot n=%d state=%d packets=%llu last_ms=%u", n,
        n > 0 ? snap[0].state : -1,
        n > 0 ? (unsigned long long)snap[0].packets : 0ULL,
        n > 0 ? snap[0].last_ms : 0);

    /* A third packet in the original direction keeps it ESTABLISHED. */
    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
                40000, 80);
    pkt.tcp_flags = 0x10; /* ACK */
    g_now = 1300;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    n = netfilter_conntrack_snapshot(snap, 4);
    say("track: after_ack state=%d packets=%llu", n > 0 ? snap[0].state : -1,
        n > 0 ? (unsigned long long)snap[0].packets : 0ULL);

    /* Untracked protocol and a fragment are ignored. */
    build_frame(f, &len, &pkt, 47 /* GRE */, 1, 2, 0, 0);
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("track: gre_ignored count=%d", netfilter_conntrack_count());
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, 3, 4, 1000, 2000);
    pkt.has_ports = 0;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("track: fragment_ignored count=%d", netfilter_conntrack_count());

    /* Tracking off: an existing flow is still matched, a new one is not
     * created. */
    netfilter_conntrack_set_enabled(0);
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, 9, 10, 1, 2);
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("track: disabled_count=%d", netfilter_conntrack_count());
    say("track: enabled=%d", netfilter_conntrack_enabled());
    netfilter_conntrack_set_enabled(1);
}

static void scenario_rewrite(void)
{
    uint8_t f[64];
    size_t len;
    netfilter_frame_t pkt;
    net_nat_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    netfilter_conntrack_flush();
    netfilter_nat_reset();

    /* DNAT: inbound TCP 18081 -> 10.0.2.15:18082. */
    rule.dir = NETFILTER_DIR_IN;
    rule.proto = NETFILTER_PROTO_TCP;
    rule.nat = NET_NAT_DNAT;
    rule.src_addr = NO_ADDR;
    rule.dst_addr = NO_ADDR;
    rule.src_port = NO_PORT;
    rule.dst_port = 18081;
    rule.nat_addr = 0x0a00020f;
    rule.nat_port = 18082;
    int add_rc = netfilter_nat_add_rule(&rule);
    say("nat: add_dnat=%d count=%d", add_rc, netfilter_nat_rule_count());

    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
                50000, 18081);
    g_now = 2000;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    say("nat: dnat_fwd dst=%08x dport=%u", frame_ip_dst(f), frame_dport(f));

    /* The reply leaves with the original destination restored. */
    net_conntrack_entry_t snap[4];
    int n = netfilter_conntrack_snapshot(snap, 4);
    uint32_t orig = n > 0 ? snap[0].dst_addr : 0;
    uint16_t orig_port = n > 0 ? snap[0].dst_port : 0;
    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x0a00020f, orig,
                18082, orig_port);
    pkt.tcp_flags = 0x12;
    g_now = 2100;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("nat: dnat_reply src=%08x sport=%u", frame_ip_src(f), frame_sport(f));

    /* A retransmitted SYN in the original direction must be translated too. */
    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
                50000, 18081);
    g_now = 2200;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    say("nat: dnat_retrans dst=%08x dport=%u", frame_ip_dst(f), frame_dport(f));

    /* MASQUERADE: outbound UDP from any source. */
    netfilter_nat_reset();
    netfilter_conntrack_flush();
    rule.dir = NETFILTER_DIR_OUT;
    rule.proto = NETFILTER_PROTO_UDP;
    rule.nat = NET_NAT_MASQUERADE;
    rule.src_addr = NO_ADDR;
    rule.dst_addr = NO_ADDR;
    rule.src_port = NO_PORT;
    rule.dst_port = NO_PORT;
    rule.nat_addr = NO_ADDR;
    rule.nat_port = 0;
    add_rc = netfilter_nat_add_rule(&rule);
    say("nat: add_masq=%d count=%d", add_rc, netfilter_nat_rule_count());

    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, 0x0a000202, 0x08080808,
                5353, 53);
    g_now = 2300;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("nat: masq_fwd src=%08x sport=%u", frame_ip_src(f), frame_sport(f));

    n = netfilter_conntrack_snapshot(snap, 4);
    uint32_t nat_src = n > 0 ? snap[0].nat_src_addr : 0;
    uint16_t nat_sport = n > 0 ? snap[0].nat_src_port : 0;
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, 0x08080808, nat_src, 53,
                nat_sport);
    g_now = 2400;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    say("nat: masq_reply dst=%08x dport=%u", frame_ip_dst(f), frame_dport(f));

    /* Deleting the rule must not disturb the flow: it is translated from the
     * entry now. */
    netfilter_nat_del_rule(0);
    say("nat: after_del count=%d", netfilter_nat_rule_count());
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, 0x0a000202, 0x08080808,
                5353, 53);
    g_now = 2500;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("nat: after_del_fwd src=%08x sport=%u", frame_ip_src(f),
        frame_sport(f));
}

static void scenario_icmp(void)
{
    uint8_t f[64];
    size_t len;
    netfilter_frame_t pkt;
    net_conntrack_entry_t snap[4];

    netfilter_nat_reset();
    netfilter_conntrack_flush();
    net_nat_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.dir = NETFILTER_DIR_OUT;
    rule.proto = NETFILTER_PROTO_ICMP;
    rule.nat = NET_NAT_SNAT;
    rule.nat_addr = 0x0a00020f;
    rule.nat_port = 0;
    rule.src_addr = NO_ADDR;
    rule.dst_addr = NO_ADDR;
    rule.src_port = NO_PORT;
    rule.dst_port = NO_PORT;
    netfilter_nat_add_rule(&rule);

    build_frame(f, &len, &pkt, NETFILTER_PROTO_ICMP, 0x0a000202, 0x08080808,
                0, 0);
    pkt.has_ports = 0;
    pkt.l4_off = 34;
    pkt.icmp_off = 34;
    pkt.icmp_type = NETFILTER_ICMP_ECHO_REQUEST;
    pkt.icmp_id = 0x1234;
    g_now = 3000;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    int n = netfilter_conntrack_snapshot(snap, 4);
    say("icmp: n=%d proto=%u sport=%u dport=%u nat=%u", n,
        n > 0 ? snap[0].proto : 0, n > 0 ? snap[0].src_port : 0,
        n > 0 ? snap[0].dst_port : 0, n > 0 ? snap[0].nat : 0);

    /* Reply: type complemented, same identifier, addresses swapped. */
    build_frame(f, &len, &pkt, NETFILTER_PROTO_ICMP, 0x08080808, 0x0a00020f,
                0, 0);
    pkt.has_ports = 0;
    pkt.l4_off = 34;
    pkt.icmp_off = 34;
    pkt.icmp_type = NETFILTER_ICMP_ECHO_REPLY;
    pkt.icmp_id = 0x1234;
    g_now = 3100;
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    n = netfilter_conntrack_snapshot(snap, 4);
    say("icmp: reply_entries=%d", n);
    say("icmp: reply_state=%d packets=%llu nat=%u", n > 0 ? snap[0].state : -1,
        n > 0 ? (unsigned long long)snap[0].packets : 0ULL,
        n > 0 ? snap[0].nat : 0);
}

static void scenario_capacity(void)
{
    char buf[4096];
    (void)buf;
    netfilter_conntrack_flush();
    int cap = NET_PROFILE_CONNTRACK_ENTRIES;
    say("cap: capacity=%d", cap);
    g_now = 10000;
    for (int i = 0; i < cap; i++) {
        int r = netfilter_conntrack_inject(0x0a000201, 0x0a000202,
                                           (uint16_t)(1000 + i), 80,
                                           NETFILTER_PROTO_TCP,
                                           NET_CONNTRACK_NEW);
        if (r != 0)
            say("cap: inject_fail i=%d r=%d", i, r);
        g_now += 1;
    }
    net_conntrack_stats_t st;
    netfilter_conntrack_get_stats(&st);
    say("cap: full count=%d created=%llu evicted=%llu", netfilter_conntrack_count(),
        (unsigned long long)st.created, (unsigned long long)st.evicted);

    /* One more flow evicts the least recently used (the first one). */
    g_now = 20000;
    int r = netfilter_conntrack_inject(0x0a000201, 0x0a000202, 65535, 80,
                                       NETFILTER_PROTO_TCP, NET_CONNTRACK_NEW);
    netfilter_conntrack_get_stats(&st);
    say("cap: overflow r=%d count=%d evicted=%llu lru_victim=%d", r,
        netfilter_conntrack_count(), (unsigned long long)st.evicted,
        netfilter_conntrack_lru_victim());

    /* Bad injection is refused without touching the table. */
    say("cap: bad_proto=%d bad_state=%d",
        netfilter_conntrack_inject(1, 2, 3, 4, 99, NET_CONNTRACK_NEW),
        netfilter_conntrack_inject(1, 2, 3, 4, NETFILTER_PROTO_TCP, 7));
}

static void scenario_expiry(void)
{
    netfilter_conntrack_flush();
    netfilter_conntrack_set_timeouts(100, 200, 50); /* ms */
    /* The conversion is lazy; tick at 1000/s means ms == ticks here. */
    g_now = 100000;
    netfilter_conntrack_inject(1, 2, 10, 20, NETFILTER_PROTO_UDP,
                               NET_CONNTRACK_NEW);
    g_now = 100010;
    netfilter_conntrack_inject(3, 4, 30, 40, NETFILTER_PROTO_TCP,
                               NET_CONNTRACK_NEW);
    g_now = 100020;
    netfilter_conntrack_inject(5, 6, 50, 60, NETFILTER_PROTO_TCP,
                               NET_CONNTRACK_ESTABLISHED);

    /* UDP expires after 50 ticks: at 100060 the first is expired, the TCP ones
     * are not (new=100, established=200). */
    g_now = 100060;
    unsigned freed = netfilter_conntrack_expire(0);
    net_conntrack_stats_t st;
    netfilter_conntrack_get_stats(&st);
    say("expire: t=60 freed=%u count=%d timeout=%llu sweeps=%u", freed,
        netfilter_conntrack_count(), (unsigned long long)st.timeout,
        netfilter_conntrack_sweeps());

    /* At 100115 the "new" TCP entry is gone too, the established one stays. */
    g_now = 100115;
    freed = netfilter_conntrack_expire(0);
    netfilter_conntrack_get_stats(&st);
    say("expire: t=115 freed=%u count=%d timeout=%llu", freed,
        netfilter_conntrack_count(), (unsigned long long)st.timeout);

    /* Wrap: last_ms near UINT32_MAX must not be treated as ancient. */
    netfilter_conntrack_flush();
    g_now = 0xFFFFFFF0u;
    netfilter_conntrack_inject(7, 8, 70, 80, NETFILTER_PROTO_UDP,
                               NET_CONNTRACK_NEW);
    g_now = 0x00000010u; /* wrapped, 32 ticks later */
    freed = netfilter_conntrack_expire(0);
    say("expire: wrap freed=%u count=%d (limit is 50 ticks)",
        freed, netfilter_conntrack_count());
    netfilter_conntrack_set_timeouts(0, 0, 0);
}

static void scenario_rules(void)
{
    uint8_t f[64];
    size_t len;
    netfilter_frame_t pkt;

    netfilter_nat_reset();
    netfilter_conntrack_flush();
    net_nat_rule_t r;
    int add_rc;
    say("rules: empty=%d", netfilter_nat_rule_count());

    memset(&r, 0, sizeof(r));
    const char *line = "in proto=tcp dport=80 action=dnat to=10.0.2.15 toport=8080";
    say("rules: parse_in=%d", netfilter_nat_parse_rule(line, strlen(line), &r));
    say("rules: parsed dir=%u proto=%u nat=%u dport=%u nat_addr=%08x nat_port=%u",
        r.dir, r.proto, r.nat, r.dst_port, r.nat_addr, r.nat_port);
    add_rc = netfilter_nat_add_rule(&r);
    say("rules: add1=%d count=%d", add_rc, netfilter_nat_rule_count());

    /* A UDP packet on port 80 does not match the TCP rule: the frame stays
     * untouched. */
    g_now = 4000;
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, 0x01020304, 0x05060708,
                1111, 80);
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    say("rules: udp_no_match dst=%08x dport=%u", frame_ip_dst(f),
        frame_dport(f));

    /* A TCP packet on port 80 matches and is translated. */
    g_now = 4100;
    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x01020304, 0x05060708,
                2222, 80);
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    say("rules: tcp_match dst=%08x dport=%u", frame_ip_dst(f), frame_dport(f));

    /* The same shape on the way out does not match an in rule. */
    g_now = 4200;
    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x01020304, 0x05060708,
                3333, 80);
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt);
    say("rules: out_no_match dst=%08x dport=%u", frame_ip_dst(f),
        frame_dport(f));

    /* A second rule for the same key never wins: first match wins. */
    memset(&r, 0, sizeof(r));
    r.dir = NETFILTER_DIR_IN;
    r.proto = NETFILTER_PROTO_TCP;
    r.nat = NET_NAT_DNAT;
    r.dst_port = 80;
    r.src_addr = NO_ADDR;
    r.dst_addr = NO_ADDR;
    r.src_port = NO_PORT;
    r.nat_addr = 0x01010101;
    add_rc = netfilter_nat_add_rule(&r);
    say("rules: add2=%d count=%d", add_rc, netfilter_nat_rule_count());
    g_now = 4300;
    build_frame(f, &len, &pkt, NETFILTER_PROTO_TCP, 0x01020304, 0x05060708,
                4444, 80);
    netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt);
    say("rules: first_match dst=%08x", frame_ip_dst(f));

    /* Invalid rules are refused, and the table is untouched. */
    memset(&r, 0, sizeof(r));
    r.dir = NETFILTER_DIR_OUT;
    r.nat = NET_NAT_DNAT; /* DNAT is input-only */
    r.nat_addr = 0x01010101;
    say("rules: reject_dnat_out=%d", netfilter_nat_add_rule(&r));
    memset(&r, 0, sizeof(r));
    r.dir = NETFILTER_DIR_OUT;
    r.nat = NET_NAT_SNAT; /* SNAT needs an address */
    r.nat_addr = NO_ADDR;
    r.src_addr = NO_ADDR;
    r.dst_addr = NO_ADDR;
    r.src_port = NO_PORT;
    r.dst_port = NO_PORT;
    say("rules: reject_snat_no_addr=%d", netfilter_nat_add_rule(&r));
    memset(&r, 0, sizeof(r));
    r.dir = NETFILTER_DIR_OUT;
    r.nat = NET_NAT_MASQUERADE;
    r.nat_addr = 0x01010101;
    say("rules: reject_masq_addr=%d", netfilter_nat_add_rule(&r));
    say("rules: count_after_rejects=%d", netfilter_nat_rule_count());
    say("rules: add_null=%d", netfilter_nat_add_rule(NULL));

    /* Delete shifts the rest down. */
    say("rules: del0=%d", netfilter_nat_del_rule(0));
    say("rules: count_after_del=%d", netfilter_nat_rule_count());
    say("rules: del_oob=%d", netfilter_nat_del_rule(5));
    netfilter_nat_reset();
    say("rules: reset count=%d", netfilter_nat_rule_count());
}

int main(void)
{
    netfilter_conntrack_init();
    netfilter_nat_reset();

    scenario_tracking();
    scenario_rewrite();
    scenario_icmp();
    scenario_capacity();
    scenario_expiry();
    scenario_rules();

    net_conntrack_stats_t st;
    netfilter_conntrack_get_stats(&st);
    say("final: tracked=%llu created=%llu evicted=%llu timeout=%llu",
        (unsigned long long)st.tracked, (unsigned long long)st.created,
        (unsigned long long)st.evicted, (unsigned long long)st.timeout);
    say("HARNESS OK");
    return g_fail ? 1 : 0;
}
