/*
 * Differential harness for the exact production kernel/net/filter/netfilter_nat.c.
 *
 * Both selector runs compile this same translation unit and drive the same
 * scenarios through the production entry points, so the transcripts must be
 * byte-identical: the C reference under #else and the Rust state machine in
 * kernel/net/filter/netfilter_nat.rs are compared on the real code, not on a
 * reimplementation of it.
 *
 * What the harness supplies is the primitive half only: a controllable clock
 * (so idle expiry and LRU are deterministic), the netif address a MASQUERADE
 * takes, the shared dotted-quad parser, libc, and the host lock shim.  The frame
 * builders are test code; the rewrite itself is the production
 * netfilter_rewrite.c, compiled in.
 */
#include "net/netfilter.h"
#include "net/filter/netfilter_internal.h"
#include "net/netfilter_rewrite.h"
#include "net/profile.h"

#include "core/stdio.h" /* va_list without <stdarg.h> */
#include "/usr/include/stdio.h"
#include "/usr/include/stdlib.h"
#include "/usr/include/string.h"
#include <unistd.h>
#include <pthread.h>

/* netfilter.c owns the global writer lock the NAT entry points take; the
 * harness defines it so the production paths link unmodified. */
spinlock_t g_netfilter_lock = SPINLOCK_INIT;

/* ------------------------------------------------------------------ clock */

static uint32_t g_now;

uint64_t timer_get_ticks(void)
{
    return (uint64_t)g_now * ARCH_TIMER_FREQ / 1000;
}

/* The MASQUERADE source address: a fixed interface address so the transcript
 * does not depend on any real netif state. */
uint32_t net_backend_interface_ipv4(int net_idx)
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

    /* A full state table refuses new flows and preserves every live binding. */
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

/* Assertions cover behavior that the old golden transcript could not certify.
 * Keep them independent of lifetime counter output and of dynamic port choice. */
static void require(int ok, const char *what)
{
    if (!ok) {
        say("NAPT assertion failed: %s", what);
        exit(1);
    }
}

static void scenario_napt(void)
{
    uint8_t f[64], before[64];
    size_t len;
    netfilter_frame_t pkt;
    net_nat_rule_t rule = {0};
    uint32_t clients[2] = {0x0a000a02, 0x0a000a03};
    uint16_t ports[2];
    const uint32_t remote = 0x08080808, public_ip = 0x0a00020f;
    netfilter_conntrack_flush();
    netfilter_nat_reset();
    netfilter_conntrack_set_enabled(1);
    rule.dir = NETFILTER_DIR_OUT;
    rule.proto = NETFILTER_PROTO_UDP;
    rule.nat = NET_NAT_MASQUERADE;
    rule.src_addr = rule.dst_addr = rule.nat_addr = NO_ADDR;
    rule.src_port = rule.dst_port = NO_PORT;
    require(netfilter_nat_add_rule(&rule) == 0, "install masquerade");
    for (int i = 0; i < 2; i++) {
        build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, clients[i], remote, 53000, 53);
        require(netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_IN, &pkt) == 0,
                "track ingress before SNAT");
        require(frame_ip_src(f) == clients[i], "SNAT waits for egress");
        require(netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_OUT, &pkt) == 0,
                "reserve egress binding on existing entry");
        require(frame_ip_src(f) == public_ip, "masquerade address");
        ports[i] = frame_sport(f);
    }
    require(ports[0] == 53000, "preserve source port when free");
    require(ports[1] >= 1024 && ports[1] != ports[0], "resolve client collision");
    require(netfilter_conntrack_count() == 2, "one entry per forwarded flow");
    require(netfilter_conntrack_inject(public_ip, remote, ports[0], 53,
                NETFILTER_PROTO_UDP, NET_CONNTRACK_NEW) == -28,
            "an untranslated tuple cannot steal an existing translated reply");
    for (int i = 0; i < 2; i++) {
        build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, clients[i], remote, 53000, 53);
        require(netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_OUT, &pkt) == 0,
                "retransmission accepted");
        require(frame_sport(f) == ports[i], "stable reserved port");
        build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, remote, public_ip, 53, ports[i]);
        require(netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_IN, &pkt) == 0,
                "reverse binding found");
        require(frame_ip_dst(f) == clients[i] && frame_dport(f) == 53000,
                "reply delivered to its original client");
        build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, remote, clients[i], 53, 53000);
        require(netfilter_conntrack_process(f, len, 1, NETFILTER_DIR_OUT, &pkt) == 0,
                "reply traverses egress");
        require(frame_ip_dst(f) == clients[i] && frame_sport(f) == 53,
                "reverse reply is not translated twice");
    }
    for (int i = 2; i < NET_PROFILE_CONNTRACK_ENTRIES; i++)
        require(netfilter_conntrack_inject(1, 2, (uint16_t)i, 80,
                    NETFILTER_PROTO_TCP, NET_CONNTRACK_NEW) == 0, "fill unused slots");
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, 0x0a000a04, remote, 53000, 53);
    memcpy(before, f, len);
    require(netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_OUT, &pkt) == -28,
            "full table refuses mapping");
    require(memcmp(before, f, len) == 0, "failed transaction leaves packet unchanged");
    net_conntrack_stats_t stats;
    netfilter_conntrack_get_stats(&stats);
    require(stats.evicted == 0, "live flows never evicted");
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, remote, public_ip, 53, ports[0]);
    require(netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_IN, &pkt) == 0,
            "full table still serves existing mappings");
    require(frame_ip_dst(f) == clients[0], "full table retains reply ownership");

    netfilter_conntrack_flush();
    netfilter_nat_reset();
    rule.nat = NET_NAT_SNAT;
    rule.nat_addr = public_ip;
    rule.nat_port = 41000;
    require(netfilter_nat_add_rule(&rule) == 0, "install fixed SNAT");
    for (int i = 0; i < 2; i++) {
        build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, clients[i], remote, 53000, 53);
        memcpy(before, f, len);
        int rc = netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_OUT, &pkt);
        require(rc == (i == 0 ? 0 : -28), "fixed port collision fails closed");
        if (i == 0) require(frame_sport(f) == 41000, "fixed port honored");
        else require(memcmp(before, f, len) == 0, "fixed collision no mutation");
    }
    netfilter_conntrack_flush();
    netfilter_conntrack_set_enabled(0);
    build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, clients[0], remote, 53000, 53);
    require(netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_OUT, &pkt) == -28,
            "disabled conntrack cannot bypass NAT policy");
    netfilter_conntrack_set_enabled(1);
    netfilter_nat_reset();
    say("NAPT ASSERTIONS OK");
}

#define NAPT_WORKERS 4
#define NAPT_FLOWS 4
static uint16_t concurrent_ports[NAPT_WORKERS][NAPT_FLOWS];
static void *napt_worker(void *arg)
{
    unsigned worker = (unsigned)(uintptr_t)arg;
    uint8_t f[64];
    size_t len;
    netfilter_frame_t pkt;
    for (int repeat = 0; repeat < 128; repeat++) {
        for (unsigned i = 0; i < NAPT_FLOWS; i++) {
            uint32_t client = 0x0a001000 + worker * NAPT_FLOWS + i;
            build_frame(f, &len, &pkt, NETFILTER_PROTO_UDP, client, 0x08080808, 53000, 53);
            require(netfilter_conntrack_process(f, len, 2, NETFILTER_DIR_OUT, &pkt) == 0,
                    "parallel binding transaction");
            uint16_t port = frame_sport(f);
            if (repeat == 0) concurrent_ports[worker][i] = port;
            else require(port == concurrent_ports[worker][i], "parallel binding stays stable");
        }
    }
    return NULL;
}
static void scenario_napt_concurrency(void)
{
    pthread_t threads[NAPT_WORKERS];
    netfilter_conntrack_flush();
    net_nat_rule_t rule = {0};
    rule.dir = NETFILTER_DIR_OUT;
    rule.proto = NETFILTER_PROTO_UDP;
    rule.nat = NET_NAT_MASQUERADE;
    rule.src_addr = rule.dst_addr = rule.nat_addr = NO_ADDR;
    rule.src_port = rule.dst_port = NO_PORT;
    require(netfilter_nat_add_rule(&rule) == 0, "parallel rule installed");
    for (uintptr_t i = 0; i < NAPT_WORKERS; i++)
        require(pthread_create(&threads[i], NULL, napt_worker, (void *)i) == 0,
                "start parallel worker");
    for (unsigned i = 0; i < NAPT_WORKERS; i++)
        require(pthread_join(threads[i], NULL) == 0, "join parallel worker");
    for (unsigned a = 0; a < NAPT_WORKERS * NAPT_FLOWS; a++)
        for (unsigned b = a + 1; b < NAPT_WORKERS * NAPT_FLOWS; b++)
            require(concurrent_ports[a / NAPT_FLOWS][a % NAPT_FLOWS] !=
                    concurrent_ports[b / NAPT_FLOWS][b % NAPT_FLOWS],
                    "parallel reply tuples have unique owners");
    require(netfilter_conntrack_count() == NAPT_WORKERS * NAPT_FLOWS,
            "parallel transactions do not leak entries");
    netfilter_conntrack_flush();
    require(netfilter_conntrack_count() == 0, "all parallel state reclaimed");
    netfilter_nat_reset();
    say("NAPT CONCURRENCY OK");
}

static uint16_t independent_checksum(const uint8_t *b, size_t n, uint32_t sum) {
    for (size_t i=0; i<n; ++i) sum += (uint32_t)b[i] << ((i&1) ? 0 : 8);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return (uint16_t)~sum;
}
static void fix_ip(uint8_t *ip) {
    ip[10]=ip[11]=0;
    netfilter_put16(ip+10, independent_checksum(ip, (ip[0]&15)*4, 0));
}
static void fix_udp(uint8_t *ip) {
    size_t h=(ip[0]&15)*4, size=netfilter_get16(ip+2)-h;
    uint8_t *udp=ip+h; udp[6]=udp[7]=0;
    uint32_t sum=17+size;
    for (int i=12;i<20;i+=2) sum += netfilter_get16(ip+i);
    uint16_t check=independent_checksum(udp,size,sum);
    netfilter_put16(udp+6,check ? check : 65535);
}
static void error_packet(uint8_t *out, size_t *len, const uint8_t *quote, size_t size,
                         uint8_t type, uint32_t source, netfilter_frame_t *pkt) {
    uint32_t destination=((uint32_t)quote[12]<<24)|((uint32_t)quote[13]<<16)|((uint32_t)quote[14]<<8)|quote[15];
    build_frame(out,len,pkt,1,source,destination,0,0);
    size_t total=20+8+size; *len=14+total;
    netfilter_put16(out+16,(uint16_t)total);
    memset(out+34,0,8); out[34]=type; out[35]=type==3 ? 4 : 0;
    netfilter_put16(out+40,1280);
    memcpy(out+42,quote,size);
    netfilter_put16(out+36,independent_checksum(out+34,8+size,0));
    fix_ip(out+14);
    pkt->proto=1; pkt->icmp_off=0; pkt->l4_off=0; pkt->has_ports=0;
}
static void scenario_icmp_errors(void) {
    for (int dnat=0;dnat<2;++dnat) for (int tcp=0;tcp<2;++tcp) {
        uint8_t original[128],forward[128],error[160],before[160];
        size_t length; netfilter_frame_t view;
        netfilter_conntrack_flush(); netfilter_nat_reset(); netfilter_conntrack_set_enabled(1);
        net_nat_rule_t rule={0};
        rule.dir=dnat ? NETFILTER_DIR_IN : NETFILTER_DIR_OUT;
        rule.nat=dnat ? NET_NAT_DNAT : NET_NAT_SNAT;
        rule.proto=tcp ? 6 : 17;
        rule.src_addr=rule.dst_addr=NO_ADDR; rule.src_port=rule.dst_port=NO_PORT;
        rule.nat_addr=dnat ? 0x0a010102 : 0xc0000201; rule.nat_port=42001;
        require(netfilter_nat_add_rule(&rule)==0,"ICMP rule");
        build_frame(original,&length,&view,rule.proto,0x0a000002,0xc6336402,53000,9000);
        if (tcp) {
            original[46]=0x50;
            uint32_t pseudo=6+20;
            for (int i=26;i<34;i+=2) pseudo+=netfilter_get16(original+i);
            netfilter_put16(original+50,independent_checksum(original+34,20,pseudo));
        }
        else { netfilter_put16(original+38,20); fix_udp(original+14); }
        fix_ip(original+14);
        memcpy(forward,original,length);
        forward[20]=0x20; /* MF on fragment zero: no partial NAT or CT allocation. */
        uint8_t rejected[128]; memcpy(rejected,forward,length);
        require(netfilter_conntrack_process(forward,length,0,rule.dir,&view)<0 &&
                !memcmp(forward,rejected,length) && netfilter_conntrack_count()==0,
                "NAT fragments fail before any mutation/allocation");
        memcpy(forward,original,length);
        require(netfilter_conntrack_process(forward,length,0,rule.dir,&view)==0,"ICMP create mapping");
        for (int full=0;full<2;++full) for (int type_index=0;type_index<3;++type_index) {
            const uint8_t type[3]={3,11,12}; size_t elen;
            error_packet(error,&elen,forward+14,full?40:28,type[type_index],0xcb007101,&view);
            memcpy(before,error,elen);
            netfilter_dir_t dir=dnat ? NETFILTER_DIR_OUT : NETFILTER_DIR_IN;
            require(netfilter_conntrack_process(error,elen,0,dir,&view)==0,"ICMP related translation");
            require(!memcmp(error+54,original+26,8),"ICMP quote original addresses");
            require(!memcmp(error+62,original+34,4),"ICMP quote original ports");
            require(independent_checksum(error+14,20,0)==0,"ICMP outer IP checksum");
            require(independent_checksum(error+34,elen-34,0)==0,"ICMP message checksum");
            require(independent_checksum(error+42,20,0)==0,"ICMP quote IP checksum");
            require(netfilter_get16(error+40)==1280,"ICMP next-hop MTU preserved");
            if (!tcp) require(netfilter_get16(error+68)==netfilter_get16(original+40),"ICMP quoted UDP checksum restored");
            else if (full) require(netfilter_get16(error+78)==netfilter_get16(original+50),"ICMP quoted TCP checksum restored");
            memcpy(error,before,elen); error[36]^=1; memcpy(before,error,elen);
            require(netfilter_conntrack_process(error,elen,0,dir,&view)<0 && !memcmp(error,before,elen),"ICMP bad checksum unchanged");
            for (size_t cut=0;cut<elen;++cut) {
                error_packet(error,&elen,forward+14,full?40:28,type[type_index],0xcb007101,&view);
                memcpy(before,error,elen);
                require(netfilter_conntrack_process(error,cut,0,dir,&view)<0 && !memcmp(error,before,elen),"ICMP all truncated prefixes unchanged");
            }
        }
        /* A private-host error quotes a reply after reverse NAT. */
        uint8_t reply[40]; memcpy(reply,original+14,40);
        memcpy(reply+12,original+30,4); memcpy(reply+16,original+26,4);
        memcpy(reply+20,original+36,2); memcpy(reply+22,original+34,2);
        if (!tcp) fix_udp(reply); fix_ip(reply);
        size_t elen;
        uint32_t origin=((uint32_t)reply[16]<<24)|((uint32_t)reply[17]<<16)|((uint32_t)reply[18]<<8)|reply[19];
        error_packet(error,&elen,reply,40,3,origin,&view);
        require(netfilter_conntrack_process(error,elen,0,rule.dir,&view)==0,"ICMP reverse-error translation");
        require(!memcmp(error+54,forward+30,4) && !memcmp(error+58,forward+26,4),"ICMP wire reply addresses");
        require(!memcmp(error+62,forward+36,2) && !memcmp(error+64,forward+34,2),"ICMP wire reply ports");
    }
    say("ICMP RELATED ASSERTIONS OK");
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
    scenario_napt();
    scenario_napt_concurrency();
    scenario_icmp_errors();
    say("HARNESS OK");
    return g_fail ? 1 : 0;
}
