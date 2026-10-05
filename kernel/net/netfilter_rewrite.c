/*
 * The pure half of NAT.  See kernel/include/net/netfilter_rewrite.h for why
 * this file exists separately from netfilter_nat.c; the short version is that
 * these functions are the ones a user-mode-network topology cannot exercise end
 * to end, and they are pure enough to be asserted on directly from the host.
 *
 * Deliberately free of kernel services: no lock, no clock, no table, no
 * allocation.  The one external symbol is the netif address lookup below, and it
 * is declared here rather than pulled in through net/lwip_stack.h so this file
 * compiles on the host without dragging lwIP's headers along.
 */

#include "net/netfilter_rewrite.h"

/* Implemented in kernel/net/lwip_stack.c; the host test supplies its own. */
uint32_t a20_lwip_netif_ipv4(int net_idx);

/* ------------------------------------------------------------------ checksum */

/*
 * The sign of each word matters and getting it backwards is silent: the result
 * is a plausible-looking checksum that is wrong by twice the change, which is
 * what this function did before it was measured against a full re-sum.  A TCP
 * port that goes 18081 -> 18082 has to come out 0x93bf -> 0x93be, and the
 * inverted form produced 0x93c0.  lwIP dropped every such segment, so the
 * symptom was "the rule matches and the connection never happens".
 */
uint16_t netfilter_csum_delta(uint16_t old_csum, uint32_t old_word,
                              uint32_t new_word) {
    uint32_t sum = (uint32_t)(~old_csum & 0xffffu);
    sum += (~old_word & 0xffffu) + ((~old_word >> 16) & 0xffffu);
    sum += (new_word & 0xffffu) + (new_word >> 16);
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

uint16_t netfilter_get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

void netfilter_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/*
 * UDP's checksum is at a fixed offset 6.  TCP's is at a fixed offset 16 --
 * *before* the options, not after them.  An earlier version of this function
 * added the option length from the data-offset field, on the theory that the
 * checksum "sits behind the options"; that theory is simply wrong, and the cost
 * of holding it was that every translated SYN had a correct-looking checksum
 * written over its MSS option while the real checksum field kept the value
 * computed for the pre-NAT port.  lwIP dropped every such segment as corrupt,
 * which is what made the first end-to-end DNAT run fail.
 */
uint16_t netfilter_l4_ckoff(const uint8_t *frame, size_t len,
                            const netfilter_frame_t *pkt) {
    if (pkt->proto == NETFILTER_PROTO_UDP)
        return (len >= (size_t)pkt->l4_off + 8) ? (uint16_t)(pkt->l4_off + 6)
                                                : 0;
    if (pkt->proto != NETFILTER_PROTO_TCP)
        return 0;
    if (len < (size_t)pkt->l4_off + 18)
        return 0;
    uint16_t doff_words = (uint16_t)((frame[pkt->l4_off + 12] >> 4) & 0xf);
    if (doff_words < 5)
        return 0; /* shorter than a TCP header */
    if (len < (size_t)pkt->l4_off + (size_t)doff_words * 4)
        return 0; /* options claim more than the frame has */
    return (uint16_t)(pkt->l4_off + 16);
}

/*
 * The L4 checksum covers the pseudo-header, whose only moving parts are the two
 * addresses: NAT adds and removes no payload, so the length is unchanged.
 */
static void netfilter_fix_l4(uint8_t *frame, size_t len,
                             const netfilter_frame_t *pkt, uint32_t old_src,
                             uint32_t old_dst, uint32_t new_src,
                             uint32_t new_dst) {
    uint16_t ckoff = netfilter_l4_ckoff(frame, len, pkt);
    if (!ckoff)
        return;
    uint16_t old_ck = netfilter_get16(frame + ckoff);
    if (old_ck == 0) {
        /* UDP over IPv4 may legitimately carry a zero checksum, which means
         * "not computed".  Applying a delta with no base would produce a
         * checksum that is wrong rather than merely stale, so the field is left
         * alone -- a stated boundary, not an oversight. */
        return;
    }
    uint16_t sum = netfilter_csum_delta(old_ck, old_src, new_src);
    sum = netfilter_csum_delta(sum, old_dst, new_dst);
    netfilter_put16(frame + ckoff, sum);
}

static void netfilter_fix_ip(uint8_t *frame, const netfilter_frame_t *pkt,
                             uint32_t old_addr, uint32_t new_addr) {
    size_t ckoff = (size_t)pkt->ip_off + 10;
    uint16_t sum = netfilter_csum_delta(netfilter_get16(frame + ckoff),
                                        old_addr, new_addr);
    netfilter_put16(frame + ckoff, sum);
}

/* ------------------------------------------------------------------ rewrite */

/* Rewrites the addresses and fixes up both checksums.  A no-op for whichever
 * address is unchanged, which is why the two callers can pass the parsed
 * values straight through instead of tracking what moved. */
void netfilter_set_addr(uint8_t *frame, size_t len,
                        const netfilter_frame_t *pkt, uint32_t new_src,
                        uint32_t new_dst) {
    uint8_t *ip = frame + pkt->ip_off;
    uint32_t old_src = pkt->src_addr, old_dst = pkt->dst_addr;

    if (new_src != old_src) {
        ip[12] = (uint8_t)(new_src >> 24);
        ip[13] = (uint8_t)(new_src >> 16);
        ip[14] = (uint8_t)(new_src >> 8);
        ip[15] = (uint8_t)new_src;
        netfilter_fix_ip(frame, pkt, old_src, new_src);
    }
    if (new_dst != old_dst) {
        ip[16] = (uint8_t)(new_dst >> 24);
        ip[17] = (uint8_t)(new_dst >> 16);
        ip[18] = (uint8_t)(new_dst >> 8);
        ip[19] = (uint8_t)new_dst;
        netfilter_fix_ip(frame, pkt, old_dst, new_dst);
    }
    if (new_src != old_src || new_dst != old_dst)
        netfilter_fix_l4(frame, len, pkt, old_src, old_dst, new_src, new_dst);
}

void netfilter_set_port(uint8_t *frame, size_t len,
                        const netfilter_frame_t *pkt, int is_src,
                        uint16_t new_port) {
    if (!new_port || !pkt->l4_off)
        return;
    uint8_t *l4 = frame + pkt->l4_off;
    size_t poff = is_src ? 0 : 2;
    if (poff + 2 > len - pkt->l4_off)
        return;
    uint16_t old_port = netfilter_get16(l4 + poff);
    if (new_port == old_port)
        return;
    netfilter_put16(l4 + poff, new_port);

    /*
     * The ports are ordinary header fields covered by the L4 checksum, so the
     * checksum moves with them.  They are folded as 32-bit words with the value
     * in the high half, which is the byte order the wire checksum sees them in.
     */
    uint16_t ckoff = netfilter_l4_ckoff(frame, len, pkt);
    if (!ckoff)
        return;
    uint16_t old_ck = netfilter_get16(frame + ckoff);
    if (!old_ck)
        return;
    netfilter_put16(frame + ckoff,
                    netfilter_csum_delta(old_ck, (uint32_t)old_port << 16,
                                         (uint32_t)new_port << 16));
}

uint32_t netfilter_masq_addr(int net_idx) {
    /*
     * MASQUERADE means "the address of the interface we are leaving", not "the
     * address this stack happened to put in the header".  Those differ exactly
     * when the kernel forwards, which is the case the feature exists for.  A
     * netif with no IPv4 address yet falls back to the primary netif, so a rule
     * installed before DHCP completes still translates rather than silently
     * passing traffic through untranslated.
     */
    uint32_t addr = a20_lwip_netif_ipv4(net_idx);
    if (addr)
        return addr;
    return a20_lwip_netif_ipv4(-1);
}

/* ---------------------------------------------------------------- conntrack */

void netfilter_ct_reply_tuple(const net_conntrack_entry_t *e, uint32_t *rsrc,
                              uint32_t *rdst, uint16_t *rsport,
                              uint16_t *rdport) {
    if (e->nat == NET_NAT_DNAT) {
        *rsrc = e->nat_dst_addr ? e->nat_dst_addr : e->dst_addr;
        *rsport = e->nat_dst_port ? e->nat_dst_port : e->dst_port;
    } else {
        *rsrc = e->dst_addr;
        *rsport = e->dst_port;
    }
    if (e->nat == NET_NAT_SNAT || e->nat == NET_NAT_MASQUERADE) {
        *rdst = e->nat_src_addr ? e->nat_src_addr : e->src_addr;
        *rdport = e->nat_src_port ? e->nat_src_port : e->src_port;
    } else {
        *rdst = e->src_addr;
        *rdport = e->src_port;
    }
}
