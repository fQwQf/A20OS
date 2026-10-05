/*
 * Host-side unit tests for the NAT rewrite half of netfilter.
 *
 * Compiled and run by tools/test-nat-rewrite-host.sh against the real
 * kernel/net/netfilter_rewrite.c, not a copy of it.
 *
 * Why this exists.  The end-to-end NAT gate (make smoke-netfilter-nat) can only
 * ever be a DNAT gate: QEMU's user-mode network puts the guest behind a NAT of
 * its own, and there is no second guest on the far side of a SNAT to send
 * anything back.  So for SNAT and MASQUERADE the evidence was the parser, the
 * /proc rule list, and reading the code.  These tests are the missing evidence
 * for the three things those paths actually have to get right:
 *
 *   1. The address rewrite, and that it fixes *both* checksums.  A rewrite with
 *      a correct header and a stale IP or pseudo-header checksum is dropped by
 *      the peer, which is indistinguishable from "the NAT rule did not match".
 *   2. The port rewrite, including the `toport=0` convention (keep the
 *      original).  There is no dynamic port allocation in this NAT -- a
 *      masquerade keeps the client's source port unless a rule pins one -- and
 *      the tests assert exactly that, rather than implying an allocator that
 *      does not exist.
 *   3. The reply-direction match: the tuple a translated reply arrives with,
 *      which is not the reverse of the entry's tuple and is the one thing that
 *      makes a reply findable at all.
 *
 * The checksum assertions are checked against an independent full re-sum (the
 * textbook algorithm, implemented here over the same bytes), not against
 * another incremental update -- an incremental implementation compared with
 * itself would agree with itself even when wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/netfilter.h"
#include "net/netfilter_rewrite.h"

/* ------------------------------------------------------------------ harness */

static int g_fail;
static int g_checks;

#define CHK(cond, ...)                                                         \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            printf("NAT_REWRITE_TEST: FAIL %s:%d ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------- frames */

#define ETH_HDR 14
#define IP_HDR 20
#define UDP_HDR 8
#define TCP_HDR 20

#define IP_OFF ETH_HDR
#define L4_OFF (ETH_HDR + IP_HDR)

/* Checksums here are computed by the independent routine below, so a frame is
 * always internally consistent before the rewrite runs. */
static uint32_t ip4(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Textbook ones-complement sum, used only as the oracle. */
static uint16_t full_sum(const uint8_t *data, size_t len, uint32_t seed) {
    uint32_t sum = seed;
    size_t i;
    for (i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (i < len)
        sum += (uint32_t)(data[i] << 8);
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

static void ip_checksum(uint8_t *ip) {
    put16(ip + 10, 0);
    put16(ip + 10, full_sum(ip, IP_HDR, 0));
}

/*
 * The L4 checksum covers the pseudo-header, so the oracle needs the addresses
 * *as they appear in the frame right now*.  That is the whole point: after a
 * rewrite the frame's addresses have changed, and a stale checksum is what the
 * peer drops.
 */
static void l4_checksum(uint8_t *frame, size_t len, uint8_t proto,
                        size_t l4_off, size_t l4_len) {
    uint8_t *ip = frame + IP_OFF;
    size_t ckoff = (proto == 17) ? l4_off + 6 : l4_off + 16;
    uint32_t seed = 0;
    seed += (uint32_t)((ip[12] << 8) | ip[13]);
    seed += (uint32_t)((ip[14] << 8) | ip[15]);
    seed += (uint32_t)((ip[16] << 8) | ip[17]);
    seed += (uint32_t)((ip[18] << 8) | ip[19]);
    seed += proto;
    seed += (uint32_t)l4_len;
    while (seed >> 16)
        seed = (seed & 0xffffu) + (seed >> 16);
    put16(frame + ckoff, 0);
    put16(frame + ckoff, full_sum(frame + l4_off, l4_len, seed));
    (void)len;
}

/* Builds an Ethernet + IPv4 + L4 frame and fills in `pkt`. */
static size_t build_frame(uint8_t *buf, netfilter_frame_t *pkt, uint8_t proto,
                          const char *src, const char *dst, uint16_t sport,
                          uint16_t dport) {
    size_t l4_len = (proto == 17) ? UDP_HDR : TCP_HDR;
    size_t len = ETH_HDR + IP_HDR + l4_len;

    memset(buf, 0, len);
    /* Ethernet: a type is all the rewriter looks at, but keep it plausible. */
    buf[12] = 0x08;
    buf[13] = 0x00;

    uint8_t *ip = buf + IP_OFF;
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(len - ETH_HDR));
    ip[8] = 64; /* TTL */
    ip[9] = proto;
    ip[12] = (uint8_t)(ip4(src) >> 24);
    ip[13] = (uint8_t)(ip4(src) >> 16);
    ip[14] = (uint8_t)(ip4(src) >> 8);
    ip[15] = (uint8_t)ip4(src);
    ip[16] = (uint8_t)(ip4(dst) >> 24);
    ip[17] = (uint8_t)(ip4(dst) >> 16);
    ip[18] = (uint8_t)(ip4(dst) >> 8);
    ip[19] = (uint8_t)ip4(dst);
    ip_checksum(ip);

    uint8_t *l4 = buf + L4_OFF;
    put16(l4 + 0, sport);
    put16(l4 + 2, dport);
    if (proto == 17) {
        put16(l4 + 4, (uint16_t)(UDP_HDR - 8));
    } else {
        buf[L4_OFF + 12] = 0x50; /* data offset 5 words, no options */
        buf[L4_OFF + 13] = 0x02; /* SYN */
    }
    l4_checksum(buf, len, proto, L4_OFF, l4_len);

    memset(pkt, 0, sizeof(*pkt));
    pkt->src_addr = ip4(src);
    pkt->dst_addr = ip4(dst);
    pkt->src_port = sport;
    pkt->dst_port = dport;
    pkt->proto = proto;
    pkt->has_ports = 1;
    pkt->ip_off = IP_OFF;
    pkt->l4_off = L4_OFF;
    pkt->tcp_flags = (proto == 6) ? 0x02 : 0;
    return len;
}

/* Re-derives every checksum over the frame as it now stands. */
static int checksums_valid(const uint8_t *frame, size_t len,
                           const netfilter_frame_t *pkt) {
    uint8_t tmp[128];
    if (len > sizeof(tmp))
        return 0;
    memcpy(tmp, frame, len);
    uint8_t *ip = tmp + pkt->ip_off;
    uint16_t want_ip = get16(ip + 10);
    ip_checksum(ip);
    if (want_ip != get16(ip + 10))
        return 0;
    size_t l4_len = (pkt->proto == 17) ? UDP_HDR : TCP_HDR;
    uint16_t want_l4 = get16(tmp + pkt->l4_off + (pkt->proto == 17 ? 6 : 16));
    l4_checksum(tmp, len, pkt->proto, pkt->l4_off, l4_len);
    return want_l4 == get16(tmp + pkt->l4_off + (pkt->proto == 17 ? 6 : 16));
}

static uint32_t frame_src(const uint8_t *f, const netfilter_frame_t *pkt) {
    const uint8_t *ip = f + pkt->ip_off;
    return ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) |
           ((uint32_t)ip[14] << 8) | ip[15];
}

static uint32_t frame_dst(const uint8_t *f, const netfilter_frame_t *pkt) {
    const uint8_t *ip = f + pkt->ip_off;
    return ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
           ((uint32_t)ip[18] << 8) | ip[19];
}

/* --------------------------------------------------------------------- test */

#define CLIENT "192.168.77.5"
#define SERVER "8.8.8.8"
#define GATEWAY "10.0.2.2"

/* 1. SNAT: the source address moves to the gateway, the destination does not,
 *    and both checksums come out right. */
static void test_snat_addr(void) {
    uint8_t f[128];
    netfilter_frame_t pkt;
    size_t len = build_frame(f, &pkt, 6, CLIENT, SERVER, 44444, 443);

    netfilter_set_addr(f, len, &pkt, ip4(GATEWAY), pkt.dst_addr);

    CHK(frame_src(f, &pkt) == ip4(GATEWAY), "snat: source is the gateway");
    CHK(frame_dst(f, &pkt) == ip4(SERVER), "snat: destination untouched");
    CHK(checksums_valid(f, len, &pkt),
        "snat: IP and TCP checksums valid after the source rewrite");

    /*
     * Rewriting to the address already in the header must be a true no-op, not
     * a second delta applied to an unchanged header.  `pkt` is the *parsed*
     * view of the frame, so it has to be re-read after the rewrite -- which is
     * what the data plane does for every packet, since netfilter_parse_frame
     * runs again on each one.  Passing a stale view here is exactly the case
     * that would apply the delta twice.
     */
    uint8_t g[128];
    netfilter_frame_t gpkt = pkt;
    memcpy(g, f, len);
    gpkt.src_addr = frame_src(g, &pkt);
    gpkt.dst_addr = frame_dst(g, &pkt);
    netfilter_set_addr(g, len, &gpkt, ip4(GATEWAY), gpkt.dst_addr);
    CHK(memcmp(f, g, len) == 0,
        "snat: rewriting the same address changes nothing");
}

/* 2. DNAT, the direction the end-to-end gate does cover, asserted here too
 *    because it is the same code with the arguments mirrored. */
static void test_dnat_addr(void) {
    uint8_t f[128];
    netfilter_frame_t pkt;
    size_t len = build_frame(f, &pkt, 6, GATEWAY, CLIENT, 18081, 49152);

    netfilter_set_addr(f, len, &pkt, pkt.src_addr, ip4("192.168.77.9"));

    CHK(frame_dst(f, &pkt) == ip4("192.168.77.9"),
        "dnat: destination rewritten");
    CHK(frame_src(f, &pkt) == ip4(GATEWAY), "dnat: source untouched");
    CHK(checksums_valid(f, len, &pkt),
        "dnat: IP and TCP checksums valid after the destination rewrite");
}

/* 3. UDP, whose checksum sits at a different offset and may legally be zero. */
static void test_udp_checksum(void) {
    uint8_t f[128];
    netfilter_frame_t pkt;
    size_t len = build_frame(f, &pkt, 17, CLIENT, SERVER, 5353, 53);

    netfilter_set_addr(f, len, &pkt, ip4(GATEWAY), pkt.dst_addr);
    CHK(checksums_valid(f, len, &pkt),
        "masquerade: IP and UDP checksums valid after the rewrite");

    /* A zero UDP checksum means "not computed" and must be left alone: a delta
     * with no base produces a wrong checksum rather than a stale one. */
    put16(f + pkt.l4_off + 6, 0);
    netfilter_set_addr(f, len, &pkt, ip4("10.0.2.3"), pkt.dst_addr);
    CHK(get16(f + pkt.l4_off + 6) == 0,
        "masquerade: a zero UDP checksum is left alone, not delta'd");
}

/* 4. The port rewrite, and the toport=0 convention. */
static void test_ports(void) {
    uint8_t f[128];
    netfilter_frame_t pkt;
    size_t len = build_frame(f, &pkt, 6, CLIENT, SERVER, 44444, 443);

    netfilter_set_port(f, len, &pkt, 1, 40000);
    CHK(get16(f + pkt.l4_off + 0) == 40000,
        "toport: the source port is rewritten");
    CHK(get16(f + pkt.l4_off + 2) == 443,
        "toport: the destination port is kept");
    CHK(checksums_valid(f, len, &pkt), "toport: TCP checksum follows the port");

    netfilter_set_port(f, len, &pkt, 0, 18081);
    CHK(get16(f + pkt.l4_off + 2) == 18081,
        "toport: the destination port is rewritten");
    CHK(checksums_valid(f, len, &pkt),
        "toport: TCP checksum follows the destination port");

    uint8_t before[128];
    memcpy(before, f, len);
    netfilter_set_port(f, len, &pkt, 1, 0);
    CHK(memcmp(before, f, len) == 0, "toport=0 keeps the original port");

    netfilter_set_port(f, len, &pkt, 0, 18081);
    CHK(memcmp(before, f, len) == 0,
        "rewriting a port to the value it already has changes nothing");
}

/* 5. MASQUERADE's address choice: the interface being left, not the address
 *    that happened to be in the header. */
static uint32_t g_netif_addr[4] = {0, 0, 0, 0};
static int g_netif_queries;

/* Mirrors the real contract: -1 means "the primary netif". */
uint32_t a20_lwip_netif_ipv4(int net_idx) {
    g_netif_queries++;
    if (net_idx < 0)
        return g_netif_addr[0];
    if (net_idx >= 4)
        return 0;
    return g_netif_addr[net_idx];
}

static void test_masq_addr(void) {
    g_netif_queries = 0;
    g_netif_addr[0] = 0;
    g_netif_addr[1] = 0;
    CHK(netfilter_masq_addr(1) == 0,
        "masquerade: an interface with no address falls back to the primary");
    CHK(g_netif_queries == 2, "masquerade: exactly one fallback lookup");

    g_netif_addr[0] = ip4(GATEWAY);
    g_netif_addr[1] = ip4("192.168.77.1");
    CHK(netfilter_masq_addr(1) == ip4("192.168.77.1"),
        "masquerade: takes the address of the interface being left");
    CHK(netfilter_masq_addr(1) != ip4(CLIENT),
        "masquerade: does not keep the client's own address");

    g_netif_addr[1] = 0;
    CHK(netfilter_masq_addr(1) == ip4(GATEWAY),
        "masquerade: an unnumbered interface falls back to the primary");
}

/* 6. The reply tuple, which is what makes a translated reply findable. */
static void test_reply_tuple(void) {
    net_conntrack_entry_t e;
    uint32_t rsrc, rdst;
    uint16_t rsport, rdport;

    memset(&e, 0, sizeof(e));
    e.src_addr = ip4(CLIENT);
    e.dst_addr = ip4(SERVER);
    e.src_port = 44444;
    e.dst_port = 443;

    /* Untranslated: the reply is the plain reverse. */
    e.nat = NET_NAT_NONE;
    netfilter_ct_reply_tuple(&e, &rsrc, &rdst, &rsport, &rdport);
    CHK(rsrc == ip4(SERVER) && rsport == 443,
        "reply (untranslated): from the server");
    CHK(rdst == ip4(CLIENT) && rdport == 44444,
        "reply (untranslated): to the client's own port");

    /* SNAT with no toport: the reply is addressed to the client's port but
     * comes *from* the address the client dialled, and arrives at the gateway
     * address the translation introduced. */
    e.nat = NET_NAT_SNAT;
    e.nat_src_addr = ip4(GATEWAY);
    e.nat_src_port = 0;
    netfilter_ct_reply_tuple(&e, &rsrc, &rdst, &rsport, &rdport);
    CHK(rsrc == ip4(SERVER) && rsport == 443,
        "reply (snat): source is unchanged by the translation");
    CHK(rdst == ip4(GATEWAY),
        "reply (snat): destination is the translated source");
    CHK(rdport == 44444,
        "reply (snat): toport=0 means the reply lands on the original port");

    /* With a pinned port, both halves move. */
    e.nat_src_port = 40000;
    netfilter_ct_reply_tuple(&e, &rsrc, &rdst, &rsport, &rdport);
    CHK(rdst == ip4(GATEWAY) && rdport == 40000,
        "reply (snat toport): destination is the translated address and port");

    /* MASQUERADE is the same shape with the address supplied by the interface. */
    e.nat = NET_NAT_MASQUERADE;
    e.nat_src_addr = ip4("192.168.77.1");
    e.nat_src_port = 0;
    netfilter_ct_reply_tuple(&e, &rsrc, &rdst, &rsport, &rdport);
    CHK(rdst == ip4("192.168.77.1") && rdport == 44444,
        "reply (masquerade): destination is the masquerade address");

    /* DNAT: the reply comes from the address the flow was forwarded to. */
    memset(&e, 0, sizeof(e));
    e.src_addr = ip4("192.168.77.5");
    e.dst_addr = ip4("192.168.77.9");
    e.src_port = 49152;
    e.dst_port = 18081;
    e.nat = NET_NAT_DNAT;
    e.nat_dst_addr = ip4("192.168.77.9");
    e.nat_dst_port = 0;
    netfilter_ct_reply_tuple(&e, &rsrc, &rdst, &rsport, &rdport);
    CHK(rsrc == ip4("192.168.77.9") && rsport == 18081,
        "reply (dnat): source is the forwarded-to address and port");
    CHK(rdst == ip4("192.168.77.5") && rdport == 49152,
        "reply (dnat): destination is the original client");

    /* The reply tuple is not the reverse of the entry's tuple, which is the
     * reason the reply-direction chain exists at all. */
    e.nat_dst_port = 18082;
    netfilter_ct_reply_tuple(&e, &rsrc, &rdst, &rsport, &rdport);
    CHK(!(rsrc == e.dst_addr && rsport == e.dst_port && rdst == e.src_addr &&
          rdport == e.src_port),
        "reply (dnat): the reply tuple is not the plain reverse of the entry");
    CHK(rsport == 18082,
        "reply (dnat): only the port differs from the reverse, which is why "
        "the swapped probe alone cannot find it");

    /* The end-to-end round trip: rewrite a forward frame the way the data
     * plane would, build the reply the peer would send, and check that the
     * reply tuple derived from the entry is exactly that frame's tuple.  If
     * this disagreed, the reply would miss the table and go out untranslated. */
    uint8_t fwd[128], rev[128];
    netfilter_frame_t fpkt, rpkt;
    size_t flen = build_frame(fwd, &fpkt, 6, CLIENT, SERVER, 44444, 443);
    netfilter_set_addr(fwd, flen, &fpkt, ip4(GATEWAY), fpkt.dst_addr);
    size_t rlen = build_frame(rev, &rpkt, 6, SERVER, GATEWAY, 443, 40000);

    memset(&e, 0, sizeof(e));
    e.src_addr = ip4(CLIENT);
    e.dst_addr = ip4(SERVER);
    e.src_port = 44444;
    e.dst_port = 443;
    e.nat = NET_NAT_SNAT;
    e.nat_src_addr = ip4(GATEWAY);
    e.nat_src_port = 40000;
    netfilter_ct_reply_tuple(&e, &rsrc, &rdst, &rsport, &rdport);
    CHK(rsrc == rpkt.src_addr && rsport == rpkt.src_port &&
            rdst == rpkt.dst_addr && rdport == rpkt.dst_port,
        "round trip: the reply tuple matches the frame the peer actually sent");

    /* ... and applying the entry to that reply restores the client's own
     * address and port, which is what the outbound half of the translation
     * does. */
    netfilter_set_addr(rev, rlen, &rpkt, rpkt.src_addr, e.src_addr);
    netfilter_set_port(rev, rlen, &rpkt, 0, e.src_port);
    CHK(frame_dst(rev, &rpkt) == ip4(CLIENT) &&
            get16(rev + rpkt.l4_off + 2) == 44444,
        "round trip: the reply leaves addressed to the original client port");
    CHK(checksums_valid(rev, rlen, &rpkt),
        "round trip: the reply's checksums are valid after being rewritten");
}

int main(void) {
    test_snat_addr();
    test_dnat_addr();
    test_udp_checksum();
    test_ports();
    test_masq_addr();
    test_reply_tuple();

    if (g_fail) {
        printf("NAT_REWRITE_TEST: FAIL %d of %d checks\n", g_fail, g_checks);
        return 1;
    }
    printf("NAT_REWRITE_TEST: PASS %d checks\n", g_checks);
    return 0;
}
