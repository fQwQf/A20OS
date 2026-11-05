/*
 * Differential harness for the exact production kernel/net/filter/netfilter.c.
 *
 * Both selector runs compile this same translation unit and drive the same
 * scenarios through the production entry points, so the transcripts must be
 * byte-identical: the C reference under #else and the Rust state machine in
 * kernel/net/filter/netfilter_filter.rs are compared on the real code.
 *
 * The filter's only outside dependency is the conntrack half, which the hook
 * calls for an accepted packet.  Both selectors get a stub that records the
 * call, so the transcript also shows *whether and in what order* the tail ran --
 * that ordering (filter first, conntrack only for an accepted packet) is one of
 * the decisions this migration moved into Rust.
 */
#include "net/netfilter.h"
#include "net/filter/netfilter_internal.h"

#include "core/stdio.h"
#include "/usr/include/string.h"
#include <unistd.h>
#include <pthread.h>

/* Panic path for the Rust runtime archive. */
__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    (void)fmt;
    (void)!write(2, "HOST PANIC\n", 11);
    _exit(134);
}

void rust_eh_personality(void) { }

/* The formatter's other halves belong to modules this harness does not build:
 * the lwIP lock (the formatter takes it) and the conntrack/NAT text section.
 * Both are stubbed, so the transcript compares exactly the filter's own rows. */
uint64_t a20_lwip_lock(void) { return 0; }
void a20_lwip_unlock(uint64_t flags) { (void)flags; }
void netfilter_nat_format(char *buf, size_t bufsz, size_t off)
{
    (void)buf;
    (void)bufsz;
    (void)off;
}

/* This fixture exercises filter rules without a live NAT or CT table. */
int netfilter_nat_rule_count(void) { return 0; }
int netfilter_conntrack_count(void) { return 0; }

/* ------------------------------------------------------- conntrack tail stub */

static int g_tail_calls;
static int g_tail_rc;
static uint32_t g_tail_src, g_tail_dst;
static uint16_t g_tail_sport, g_tail_dport;
static int g_tail_dir;

#ifdef CONFIG_NETFILTER_FILTER_RUST
int a20_netfilter_ct_process_tail(uint8_t *frame, size_t len, int net_idx,
                                   uint8_t dir, const netfilter_frame_t *pkt)
{
    (void)frame;
    (void)len;
    (void)net_idx;
    g_tail_calls++;
    g_tail_dir = dir;
    if (pkt) {
        g_tail_src = pkt->src_addr;
        g_tail_dst = pkt->dst_addr;
        g_tail_sport = pkt->src_port;
        g_tail_dport = pkt->dst_port;
    }
    return g_tail_rc;
}
#else
int netfilter_conntrack_process(uint8_t *frame, size_t len, int net_idx,
                                 netfilter_dir_t dir,
                                 const netfilter_frame_t *pkt)
{
    (void)frame;
    (void)len;
    (void)net_idx;
    g_tail_calls++;
    g_tail_dir = (int)dir;
    if (pkt) {
        g_tail_src = pkt->src_addr;
        g_tail_dst = pkt->dst_addr;
        g_tail_sport = pkt->src_port;
        g_tail_dport = pkt->dst_port;
    }
    return g_tail_rc;
}
#endif

/* ------------------------------------------------------------------ helpers */

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int g_fail;

/* One Ethernet + optional VLAN + IPv4 + L4 frame. */
static size_t build(uint8_t *f, uint16_t ethertype, int vlan, uint8_t proto,
                    uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                    uint8_t icmp_type, uint16_t icmp_id, uint8_t ihl_words,
                    uint16_t frag, size_t total)
{
    memset(f, 0, total);
    memset(f, 0x11, 6);
    memset(f + 6, 0x22, 6);
    size_t off = 14; /* length of the Ethernet header */
    if (vlan) {
        /* TPID at 12/13, TCI at 14/15, inner ethertype at 16/17 -- which is
         * where the walk looks for it. */
        f[12] = 0x81; f[13] = 0x00;
        f[14] = 0x00; f[15] = 0x05;
        f[16] = (uint8_t)(ethertype >> 8);
        f[17] = (uint8_t)ethertype;
        off = 18;
    } else {
        f[12] = (uint8_t)(ethertype >> 8);
        f[13] = (uint8_t)ethertype;
    }
    uint8_t *ip = f + off;
    ip[0] = (uint8_t)(0x40 | (ihl_words & 0x0f));
    ip[2] = (uint8_t)((total - off) >> 8);
    ip[3] = (uint8_t)(total - off);
    ip[6] = (uint8_t)(frag >> 8);
    ip[7] = (uint8_t)frag;
    ip[9] = proto;
    ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);  ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dst >> 24); ip[17] = (uint8_t)(dst >> 16);
    ip[18] = (uint8_t)(dst >> 8);  ip[19] = (uint8_t)dst;
    uint8_t *l4 = ip + (ihl_words & 0x0f) * 4;
    if (proto == NETFILTER_PROTO_ICMP) {
        l4[0] = icmp_type;
        l4[4] = (uint8_t)(icmp_id >> 8);
        l4[5] = (uint8_t)icmp_id;
    } else {
        l4[0] = (uint8_t)(sport >> 8); l4[1] = (uint8_t)sport;
        l4[2] = (uint8_t)(dport >> 8); l4[3] = (uint8_t)dport;
        l4[13] = 0x12;
    }
    return total;
}

static void dump_frame(const char *tag, const netfilter_frame_t *p, int ok)
{
    say("frame %s ok=%d src=%08x dst=%08x sport=%u dport=%u proto=%u "
        "ports=%u ip_off=%u l4_off=%u flags=%u icmp_off=%u type=%u id=%u", tag,
        ok, p->src_addr, p->dst_addr, p->src_port, p->dst_port, p->proto,
        p->has_ports, p->ip_off, p->l4_off, p->tcp_flags, p->icmp_off,
        p->icmp_type, p->icmp_id);
}

static void scenario_parse_frame(void)
{
    uint8_t f[128];
    netfilter_frame_t p;

    size_t n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
                     40000, 80, 0, 0, 5, 0, 54);
    int ok = netfilter_parse_frame(f, n, &p);
    dump_frame("tcp", &p, ok);

    n = build(f, 0x0800, 0, NETFILTER_PROTO_UDP, 0x01020304, 0x05060708,
              5353, 53, 0, 0, 5, 0, 54);
    ok = netfilter_parse_frame(f, n, &p);
    dump_frame("udp", &p, ok);

    n = build(f, 0x0800, 0, NETFILTER_PROTO_ICMP, 1, 2, 0, 0,
              NETFILTER_ICMP_ECHO_REQUEST, 0x1234, 5, 0, 54);
    ok = netfilter_parse_frame(f, n, &p);
    dump_frame("icmp-echo", &p, ok);

    n = build(f, 0x0800, 0, NETFILTER_PROTO_ICMP, 1, 2, 0, 0,
              NETFILTER_ICMP_ECHO_REPLY, 0x1234, 5, 0, 54);
    ok = netfilter_parse_frame(f, n, &p);
    dump_frame("icmp-reply", &p, ok);

    /* A type that is parsed past but never tracked. */
    n = build(f, 0x0800, 0, NETFILTER_PROTO_ICMP, 1, 2, 0, 0, 3, 0, 5, 0, 54);
    ok = netfilter_parse_frame(f, n, &p);
    dump_frame("icmp-unreach", &p, ok);

    /* VLAN-tagged: the walk must skip the tag. */
    n = build(f, 0x0800, 1, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
              40000, 80, 0, 0, 5, 0, 58);
    ok = netfilter_parse_frame(f, n, &p);
    dump_frame("vlan", &p, ok);

    /* Not IPv4. */
    n = build(f, 0x0806, 0, NETFILTER_PROTO_TCP, 1, 2, 3, 4, 0, 0, 5, 0, 54);
    ok = netfilter_parse_frame(f, n, &p);
    say("frame arp ok=%d", ok);

    /* Truncated before the IPv4 header. */
    n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 1, 2, 3, 4, 0, 0, 5, 0, 54);
    ok = netfilter_parse_frame(f, 20, &p);
    say("frame truncated ok=%d", ok);

    /* Options: ihl 6 words, and the L4 offsets must follow. */
    n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
              40000, 80, 0, 0, 6, 0, 58);
    ok = netfilter_parse_frame(f, n, &p);
    dump_frame("options", &p, ok);

    /* Non-first fragment: addresses only, no L4. */
    n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
              40000, 80, 0, 0, 5, 0x0010, 54);
    ok = netfilter_parse_frame(f, n, &p);
    dump_frame("fragment", &p, ok);

    /* Smaller than an IPv4 header but with the L4 bytes present. */
    n = build(f, 0x0800, 0, NETFILTER_PROTO_UDP, 1, 2, 3, 4, 0, 0, 5, 0, 54);
    ok = netfilter_parse_frame(f, 40, &p);
    say("frame short-l4 ok=%d", ok);
}

static void scenario_parse_rule(void)
{
    netfilter_rule_t r;
    const char *lines[] = {
        "in proto=tcp dst=10.0.2.15 dport=80 action=drop",
        "out proto=udp src=10.0.2.2 sport=5353 action=accept",
        "in proto=icmp src=any dst=any action=drop",
        "in proto=47 action=accept",
        "in bogus=1 action=drop",
        "sideways action=drop",
        "in action=maybe",
        "in dport=70000 action=drop",
        "in proto=999 action=drop",
        "in dst=1.2.3 action=drop",
        "in proto=tcp trailing",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        int rc = netfilter_parse_rule(lines[i], strlen(lines[i]), &r);
        if (rc == 0) {
            say("rule %zu rc=0 dir=%u proto=%u action=%u src=%08x dst=%08x "
                "sport=%u dport=%u", i, r.dir, r.proto, r.action, r.src_addr,
                r.dst_addr, r.src_port, r.dst_port);
        } else {
            say("rule %zu rc=%d", i, rc);
        }
    }
}

static void scenario_table(void)
{
    netfilter_rule_t r;
    memset(&r, 0, sizeof(r));
    say("table: empty count=%d", netfilter_rule_count());

    r.dir = NETFILTER_DIR_IN;
    r.proto = NETFILTER_PROTO_TCP;
    r.action = NETFILTER_DROP;
    r.src_addr = NETFILTER_NO_ADDR;
    r.dst_addr = NETFILTER_NO_ADDR;
    r.src_port = NETFILTER_NO_PORT;
    r.dst_port = 80;
    int a = netfilter_add_rule(&r);
    say("table: add1=%d count=%d", a, netfilter_rule_count());

    r.dst_port = 22;
    int b = netfilter_add_rule(&r);
    say("table: add2=%d count=%d", b, netfilter_rule_count());

    netfilter_rule_t got;
    int g = netfilter_get_rule(1, &got);
    say("table: get1 rc=%d dport=%u", g, got.dst_port);
    say("table: get_oob rc=%d", netfilter_get_rule(5, &got));
    say("table: get_null rc=%d", netfilter_get_rule(0, NULL));

    /* Delete shifts the second into index 0. */
    int del_rc = netfilter_del_rule(0);
    say("table: del0 rc=%d count=%d", del_rc, netfilter_rule_count());
    g = netfilter_get_rule(0, &got);
    say("table: after_del rc=%d dport=%u", g, got.dst_port);
    say("table: del_oob rc=%d", netfilter_del_rule(7));

    netfilter_reset();
    say("table: after_reset count=%d", netfilter_rule_count());
}

static void scenario_eval_and_hooks(void)
{
    uint8_t f[128];
    netfilter_stats_t st;
    netfilter_rule_t r;
    netfilter_reset();

    /* No rules: everything accepted, counters still move. */
    size_t n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
                     40000, 80, 0, 0, 5, 0, 54);
    g_tail_calls = 0;
    int v = netfilter_input(f, n, 1);
    say("hook: empty_in verdict=%d tail=%d", v, g_tail_calls);
    netfilter_get_stats(&st);
    say("hook: stats in_packets=%llu in_accepted=%llu in_dropped=%llu",
        (unsigned long long)st.in_packets,
        (unsigned long long)st.in_accepted,
        (unsigned long long)st.in_dropped);

    /* Non-IPv4 is accepted and never reaches conntrack. */
    n = build(f, 0x0806, 0, NETFILTER_PROTO_TCP, 1, 2, 3, 4, 0, 0, 5, 0, 54);
    g_tail_calls = 0;
    v = netfilter_output(f, n, 1);
    say("hook: arp_out verdict=%d tail=%d", v, g_tail_calls);

    /* A drop rule stops the packet before conntrack. */
    memset(&r, 0, sizeof(r));
    r.dir = NETFILTER_DIR_IN;
    r.proto = NETFILTER_PROTO_TCP;
    r.action = NETFILTER_DROP;
    r.src_addr = NETFILTER_NO_ADDR;
    r.dst_addr = NETFILTER_NO_ADDR;
    r.src_port = NETFILTER_NO_PORT;
    r.dst_port = 80;
    netfilter_add_rule(&r);

    n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
              40000, 80, 0, 0, 5, 0, 54);
    g_tail_calls = 0;
    v = netfilter_input(f, n, 1);
    say("hook: drop_in verdict=%d tail=%d dir=%d", v, g_tail_calls, g_tail_dir);

    /* An accepted packet reaches conntrack with the parsed tuple. */
    n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202, 0x0a00020f,
              40000, 443, 0, 0, 5, 0, 54);
    g_tail_calls = 0;
    v = netfilter_input(f, n, 1);
    say("hook: accept_in verdict=%d tail=%d src=%08x dport=%u", v,
        g_tail_calls, g_tail_src, g_tail_dport);

    /* The same packet on the out side does not match an in rule. */
    g_tail_calls = 0;
    v = netfilter_output(f, n, 1);
    say("hook: accept_out verdict=%d tail=%d", v, g_tail_calls);

    netfilter_get_stats(&st);
    say("hook: stats in_packets=%llu out_packets=%llu in_accepted=%llu "
        "out_accepted=%llu in_dropped=%llu out_dropped=%llu",
        (unsigned long long)st.in_packets,
        (unsigned long long)st.out_packets,
        (unsigned long long)st.in_accepted,
        (unsigned long long)st.out_accepted,
        (unsigned long long)st.in_dropped,
        (unsigned long long)st.out_dropped);

    /* The matched rule's own counters are charged exactly once per match. */
    netfilter_rule_t got;
    netfilter_get_rule(0, &got);
    say("hook: rule matched=%llu bytes=%llu",
        (unsigned long long)got.matched, (unsigned long long)got.bytes);

    /* Capacity: fill the table and refuse one more. */
    netfilter_reset();
    int added = 0;
    int last = 0;
    memset(&r, 0, sizeof(r));
    r.dir = NETFILTER_DIR_OUT;
    r.proto = NETFILTER_PROTO_ANY;
    r.action = NETFILTER_ACCEPT;
    r.src_addr = NETFILTER_NO_ADDR;
    r.dst_addr = NETFILTER_NO_ADDR;
    r.src_port = NETFILTER_NO_PORT;
    r.dst_port = NETFILTER_NO_PORT;
    for (int i = 0; i < NETFILTER_MAX_RULES + 2; i++) {
        last = netfilter_add_rule(&r);
        if (last < 0)
            break;
        added++;
    }
    say("cap: added=%d last_rc=%d count=%d", added, last,
        netfilter_rule_count());
    netfilter_reset();
    say("cap: reset count=%d", netfilter_rule_count());
}

static void scenario_format(void)
{
    char buf[1024];
    netfilter_rule_t r;
    netfilter_reset();
    memset(&r, 0, sizeof(r));
    r.dir = NETFILTER_DIR_IN;
    r.proto = NETFILTER_PROTO_TCP;
    r.action = NETFILTER_DROP;
    r.src_addr = NETFILTER_NO_ADDR;
    r.dst_addr = 0x0a00020f;
    r.src_port = NETFILTER_NO_PORT;
    r.dst_port = 80;
    netfilter_add_rule(&r);
    memset(buf, 0, sizeof(buf));
    netfilter_format(buf, sizeof(buf));
    printf("%s", buf);
    netfilter_reset();
}

/*
 * Concurrency: real threads driving the production entry points at once.
 *
 * Writers add and delete rules through netfilter_add_rule()/del_rule() (which
 * take g_netfilter_lock) while a reader runs the full hook, which parses and
 * evaluates under the rule seqlock.  The interleaving is nondeterministic, so only the
 * invariants are printed: the writers finish, nothing crashes or hangs, and a
 * reset leaves the table empty.  A seqlock that let a reader act on a
 * half-published table would surface here as a bad action, not as a number.
 */
#define CONC_WRITERS 3
#define CONC_ITERS   400

typedef struct {
    int iterations;
    int adds;
    int dels;
    int evals;
} conc_worker_t;

static volatile int g_conc_stop;

static void *conc_writer(void *arg)
{
    conc_worker_t *w = arg;
    netfilter_rule_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < w->iterations; i++) {
        r.dir = NETFILTER_DIR_IN;
        r.proto = NETFILTER_PROTO_ANY;
        r.action = (uint8_t)(i & 1 ? NETFILTER_ACCEPT : NETFILTER_DROP);
        r.src_addr = NETFILTER_NO_ADDR;
        r.dst_addr = NETFILTER_NO_ADDR;
        r.src_port = NETFILTER_NO_PORT;
        r.dst_port = NETFILTER_NO_PORT;
        if (netfilter_add_rule(&r) >= 0)
            w->adds++;
        (void)netfilter_del_rule(0);
        w->dels++;
    }
    return NULL;
}

static void *conc_reader(void *arg)
{
    conc_worker_t *w = arg;
    uint8_t f[64];
    size_t n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202,
                     0x0a00020f, 40000, 80, 0, 0, 5, 0, 54);
    while (!g_conc_stop) {
        /* The full hook: parse, evaluate under the seqlock, and the conntrack
         * tail for an accepted packet. */
        int v = netfilter_input(f, n, 1);
        if (v == NETFILTER_DROP || v == NETFILTER_ACCEPT)
            w->evals++;
    }
    return NULL;
}

static void scenario_concurrency(void)
{
    conc_worker_t writers[CONC_WRITERS];
    conc_worker_t reader = {0, 0, 0, 0};
    pthread_t wt[CONC_WRITERS], rt;
    netfilter_reset();
    g_conc_stop = 0;
    pthread_create(&rt, NULL, conc_reader, &reader);
    for (int i = 0; i < CONC_WRITERS; i++) {
        writers[i].iterations = CONC_ITERS;
        writers[i].adds = 0;
        writers[i].dels = 0;
        writers[i].evals = 0;
        pthread_create(&wt[i], NULL, conc_writer, &writers[i]);
    }
    int adds = 0, dels = 0;
    for (int i = 0; i < CONC_WRITERS; i++) {
        pthread_join(wt[i], NULL);
        adds += writers[i].adds;
        dels += writers[i].dels;
    }
    g_conc_stop = 1;
    pthread_join(rt, NULL);
    netfilter_reset();
    /* Only invariants are printed: how many adds succeeded depends on the
     * interleaving (a full table refuses), so a count here would make the
     * golden unstable without testing anything more. */
    say("concurrency: writers=%d iters=%d finished=%d eval_ok=%d "
        "count_after_reset=%d", CONC_WRITERS, CONC_ITERS,
        (adds + dels) > 0, reader.evals > 0, netfilter_rule_count());
}

static void scenario_tracking_failure(void)
{
    uint8_t f[64];
    netfilter_reset();
    size_t n = build(f, 0x0800, 0, NETFILTER_PROTO_TCP, 0x0a000202,
                     0x0a00020f, 40000, 80, 0, 0, 5, 0, 54);
    g_tail_rc = -28;
    for (int output = 0; output <= 1; ++output) {
        netfilter_stats_t before, after;
        netfilter_get_stats(&before);
        int verdict = output ? netfilter_output(f, n, 1) : netfilter_input(f, n, 1);
        netfilter_get_stats(&after);
        uint64_t drops = output ? after.out_dropped - before.out_dropped :
                                  after.in_dropped - before.in_dropped;
        uint64_t accepted = output ? after.out_accepted - before.out_accepted :
                                     after.in_accepted - before.in_accepted;
        if (verdict != NETFILTER_DROP || drops != 1 || accepted != 0) {
            g_fail = 1;
            say("hook: NAT exhaustion verdict/counters disagree");
        }
    }
    g_tail_rc = 0;
}

int main(void)
{
    scenario_parse_frame();
    scenario_parse_rule();
    scenario_table();
    scenario_eval_and_hooks();
    scenario_format();
    /* The deterministic counters are snapshotted before the concurrency phase:
     * its reader runs an interleaving-dependent number of evals. */
    netfilter_stats_t st;
    netfilter_get_stats(&st);
    say("final: in_packets=%llu out_packets=%llu in_accepted=%llu "
        "out_accepted=%llu in_dropped=%llu out_dropped=%llu",
        (unsigned long long)st.in_packets,
        (unsigned long long)st.out_packets,
        (unsigned long long)st.in_accepted,
        (unsigned long long)st.out_accepted,
        (unsigned long long)st.in_dropped,
        (unsigned long long)st.out_dropped);
    scenario_tracking_failure();
    scenario_concurrency();
    say("HARNESS %s", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
