/* Non-DPDK host boundary for the real upstream ff_veth.c. No rte_* API is
 * emulated: RX tokens own copied class frames and TX uses synchronous class
 * borrow semantics. Everything here runs on the single BSD executor. */
#include <sys/ctype.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/eventhandler.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/socketvar.h>
#include <sys/proc.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_llatbl.h>
#include <net/ethernet.h>
#include <net/pfil.h>
#include <net/route.h>
#include <net/route/route_ctl.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>
#include <netinet/ip.h>
#include <machine/in_cksum.h>
#include <netinet/ip_var.h>
#include <netinet6/ip6_var.h>
#include <net/vnet.h>
#include "ff_veth.h"
#include "ff_config.h"
#include "ff_dpdk_if.h"
#include "ff_host_interface.h"

/* Mirror protocol.h without importing A20OS headers into the BSD namespace. */
typedef struct a20_bsd_nic_config {
    int port_id;
    char ifname[16];
    uint8_t mac[6];
    char address[16];
    char netmask[16];
    char broadcast[16];
    char gateway[16];
} a20_bsd_nic_config_t;
typedef struct a20_bsd_nic_stats {
    uint64_t rx_frames, tx_frames, rx_drops, tx_drops, tx_errors, rx_starved;
} a20_bsd_nic_stats_t;

#define NIC_FRAME_BYTES 1536
#define NIC_RX_TOKENS 32

extern int port_host_nic_bind(void *context, int port_id);
extern void port_host_nic_unbind(void *context);
extern int port_host_nic_recv(void *context, void *frame, int capacity);
extern int port_host_nic_send(void *context, const void *frame, int length);
extern void port_host_nic_poll(void *context);
extern void port_host_nic_token_freed(void *context);
/* A20OS policy bridge; resolves to the non-isolated common object. */
extern void a20_bsd_capture_input(const void *frame, size_t len, int ifindex);
extern int a20_bsd_netfilter_busy(void);
extern int a20_bsd_netfilter_input(void *frame, size_t len, int ifindex);
extern int a20_bsd_netfilter_output(void *frame, size_t len, int ifindex);
/* Mirror of netfilter_action_t: 0 accept, 1 drop. */
#define BSD_NETFILTER_DROP 1

typedef struct nic_token {
    struct nic_token *next;
    struct ff_dpdk_if_context *owner;
    uint8_t frame[NIC_FRAME_BYTES];
} nic_token_t;

struct ff_dpdk_if_context {
    struct ff_dpdk_if_context *next;
    void *softc;
    void *ifp;
    int port_id;
    nic_token_t tokens[NIC_RX_TOKENS];
    nic_token_t *free_tokens;
    unsigned rx_outstanding;
    uint8_t tx_frame[NIC_FRAME_BYTES];
    a20_bsd_nic_stats_t stats;
};

static struct ff_dpdk_if_context *interfaces;

static struct ff_dpdk_if_context *nic_for_ifp(const struct ifnet *ifp)
{
    for (struct ff_dpdk_if_context *ctx = interfaces; ctx; ctx = ctx->next)
        if (ctx->ifp == ifp)
            return ctx;
    return NULL;
}

/* Common policy operates on writable Ethernet frames. L2 capture already
 * happened on RX, so only the EtherType is needed here. */
static struct mbuf *filter_ipv4(struct mbuf *m, int port_id, bool output)
{
    if (!a20_bsd_netfilter_busy())
        return m;

    m = m_unshare(m, M_NOWAIT);
    if (!m)
        return NULL;

    /* NAT updates transport checksums incrementally. Materialize a delayed
     * checksum first, while the original pseudo-header is still present. */
    if (output && (m->m_pkthdr.csum_flags & CSUM_DELAY_DATA)) {
        in_delayed_cksum(m);
        m->m_pkthdr.csum_flags &= ~CSUM_DELAY_DATA;
    }

    uint8_t small_frame[NIC_FRAME_BYTES];
    int length = m->m_pkthdr.len;
    if (length < sizeof(struct ip) || length > IP_MAXPACKET) {
        m_freem(m);
        return NULL;
    }
    size_t frame_len = ETHER_HDR_LEN + (size_t)length;
    uint8_t *frame = small_frame;
    if (frame_len > sizeof(small_frame))
        frame = malloc(frame_len, M_TEMP, M_NOWAIT);
    if (!frame) {
        m_freem(m);
        return NULL;
    }

    memset(frame, 0, ETHER_HDR_LEN);
    frame[12] = ETHERTYPE_IP >> 8;
    frame[13] = ETHERTYPE_IP & 0xff;
    m_copydata(m, 0, length, (char *)frame + ETHER_HDR_LEN);
    int verdict = output ? a20_bsd_netfilter_output(frame, frame_len, port_id)
                         : a20_bsd_netfilter_input(frame, frame_len, port_id);
    if (verdict != BSD_NETFILTER_DROP)
        m_copyback(m, 0, length, (const char *)frame + ETHER_HDR_LEN);
    if (frame != small_frame)
        free(frame, M_TEMP);
    if (verdict == BSD_NETFILTER_DROP) {
        m_freem(m);
        return NULL;
    }
    return m;
}

struct mbuf *port_ipv4_input(struct mbuf *m)
{
    struct ff_dpdk_if_context *ctx = nic_for_ifp(m->m_pkthdr.rcvif);
    if (!ctx)
        return m;

    struct ip *ip = mtod(m, struct ip *);
    if (ip->ip_off & htons(IP_MF | IP_OFFMASK)) {
        /* A long-lived fragment must not pin one of the 32 NIC RX tokens.
         * m_dup copies the data to owned BSD storage and preserves metadata. */
        struct mbuf *owned = m_dup(m, M_NOWAIT);
        m_freem(m);
        if (!owned) {
            ctx->stats.rx_drops++;
            return NULL;
        }
        m = ip_reass(owned);
        if (!m)
            return NULL;
        ip = mtod(m, struct ip *);
        ip->ip_off = 0;
        ip->ip_sum = 0;
        ip->ip_sum = in_cksum(m, ip->ip_hl << 2);
        m->m_pkthdr.csum_flags = 0;
    }
    m = filter_ipv4(m, ctx->port_id, false);
    if (!m)
        ctx->stats.rx_drops++;
    return m;
}

static pfil_return_t filter_ipv4_output(pfil_packet_t packet, struct ifnet *ifp,
                                       int flags, void *ruleset, struct inpcb *inp)
{
    (void)flags;
    (void)ruleset;
    (void)inp;
    struct ff_dpdk_if_context *ctx = nic_for_ifp(ifp);
    if (!ctx)
        return PFIL_PASS;
    *packet.m = filter_ipv4(*packet.m, ctx->port_id, true);
    if (*packet.m)
        return PFIL_PASS;
    /* The NIC path reported a policy drop as successful consumption. */
    ctx->stats.tx_drops++;
    return PFIL_CONSUMED;
}

void port_ipv4_filter_init(pfil_head_t head)
{
    struct pfil_hook_args hook_args = {
        .pa_version = PFIL_VERSION,
        .pa_flags = PFIL_OUT,
        .pa_type = PFIL_TYPE_IP4,
        .pa_func = filter_ipv4_output,
        .pa_modname = "a20",
        .pa_rulname = "ipv4",
    };
    struct pfil_link_args link_args = {
        .pa_version = PFIL_VERSION,
        .pa_flags = PFIL_OUT | PFIL_HEADPTR | PFIL_HOOKPTR,
        .pa_head = head,
        .pa_hook = pfil_add_hook(&hook_args),
    };
    if (!head || !link_args.pa_hook || pfil_link(&link_args))
        panic("unable to install A20 IPv4 egress filter");
}

static void token_return(nic_token_t *token, int protocol_released)
{
    if (!token) return;
    struct ff_dpdk_if_context *ctx = token->owner;
    token->next = ctx->free_tokens;
    ctx->free_tokens = token;
    if (ctx->rx_outstanding) ctx->rx_outstanding--;
    /* An empty device queue is not new work. Only a frame leaving protocol
     * ownership opens a slot that may still have a pending IRQ behind it. */
    if (protocol_released) port_host_nic_token_freed(ctx);
}

static nic_token_t *token_take(struct ff_dpdk_if_context *ctx)
{
    nic_token_t *token = ctx->free_tokens;
    if (!token) return NULL;
    ctx->free_tokens = token->next;
    token->next = NULL;
    ctx->rx_outstanding++;
    return token;
}

struct ff_dpdk_if_context *ff_dpdk_register_if(void *sc, void *ifp,
                                                struct ff_port_cfg *cfg)
{
    if (!cfg || cfg->port_id >= 1024) return NULL;
    struct ff_dpdk_if_context *ctx = ff_calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->softc = sc;
    ctx->ifp = ifp;
    ctx->port_id = cfg->port_id;
    for (unsigned i = 0; i < NIC_RX_TOKENS; ++i) {
        ctx->tokens[i].owner = ctx;
        ctx->tokens[i].next = ctx->free_tokens;
        ctx->free_tokens = &ctx->tokens[i];
    }
    if (port_host_nic_bind(ctx, cfg->port_id) < 0) {
        ff_free(ctx);
        return NULL;
    }
    ctx->next = interfaces;
    interfaces = ctx;
    return ctx;
}

void ff_dpdk_deregister_if(struct ff_dpdk_if_context *ctx)
{
    if (!ctx) return;
    struct ff_dpdk_if_context **entry = &interfaces;
    while (*entry && *entry != ctx)
        entry = &(*entry)->next;
    if (*entry)
        *entry = ctx->next;
    port_host_nic_unbind(ctx);
    /* A live detach must first drain protocol-owned RX mbufs. Until such a
     * teardown exists, retain the storage rather than causing token UAF. */
    if (ctx->rx_outstanding) return;
    ff_free(ctx);
}

void ff_dpdk_pktmbuf_free(void *token)
{
    token_return((nic_token_t *)token, 1);
}

int ff_dpdk_if_send(struct ff_dpdk_if_context *ctx, void *m, int total)
{
    if (!ctx || !m || total <= 0 || total > NIC_FRAME_BYTES) {
        if (m) ff_mbuf_free(m);
        return EINVAL;
    }
    struct ff_tx_offload offload;
    memset(&offload, 0, sizeof(offload));
    ff_mbuf_tx_offload(m, &offload);
    int unsupported = offload.ip_csum || offload.tcp_csum || offload.udp_csum ||
                      offload.sctp_csum || offload.tso_seg_size;
    int result = 0;
    if (unsupported || ff_mbuf_copydata(m, ctx->tx_frame, 0, total) < 0) {
        result = EOPNOTSUPP;
    } else if (port_host_nic_send(ctx, ctx->tx_frame, total) < 0) {
        result = ENOBUFS;
    } else {
        ctx->stats.tx_frames++;
    }
    if (result) ctx->stats.tx_errors++;
    /* if_transmit consumes the mbuf on every return path. The class driver has
     * copied tx_frame into its own DMA slot before host_nic_send returns. */
    ff_mbuf_free(m);
    return result;
}

void *port_nic_attach(const a20_bsd_nic_config_t *config)
{
    if (!config) return NULL;
    struct ff_port_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = (char *)config->ifname;
    cfg.ifname = (char *)config->ifname;
    cfg.port_id = (uint16_t)config->port_id;
    memcpy(cfg.mac, config->mac, sizeof(cfg.mac));
    cfg.addr = (char *)config->address;
    cfg.netmask = (char *)config->netmask;
    cfg.broadcast = (char *)config->broadcast;
    cfg.gateway = (char *)config->gateway;
    /* All offload fields, addr6 and VLAN/lcore lists deliberately remain zero. */
    void *context = ff_veth_attach(&cfg);
    if (context) {
        struct ff_dpdk_if_context *ctx = context;
        if (ctx->ifp) if_up((struct ifnet *)ctx->ifp);
    }
    return context;
}

void port_nic_progress(void *context, unsigned budget)
{
    struct ff_dpdk_if_context *ctx = context;
    if (!ctx) return;
    port_host_nic_poll(ctx);
    if (!budget) budget = 32;
    for (unsigned i = 0; i < budget; ++i) {
        nic_token_t *token = token_take(ctx);
        if (!token) {
            ctx->stats.rx_starved++;
            return;
        }
        int length = port_host_nic_recv(ctx, token->frame, NIC_FRAME_BYTES);
        if (length <= 0) {
            token_return(token, 0);
            if (length < 0) ctx->stats.rx_drops++;
            return;
        }
        if (length > NIC_FRAME_BYTES) {
            ctx->stats.rx_drops++;
            token_return(token, 0);
            continue;
        }
        a20_bsd_capture_input(token->frame, (size_t)length, ctx->port_id);
        void *m = ff_mbuf_gethdr(token, (uint16_t)length, token->frame,
                                 (uint16_t)length, 0);
        if (!m) {
            ctx->stats.rx_drops++;
            token_return(token, 0);
            continue;
        }
        ctx->stats.rx_frames++;
        /* FreeBSD owns both mbuf and token after this call. */
        ff_veth_process_packet(ctx->ifp, m);
    }
}

void port_nic_stats(void *context, a20_bsd_nic_stats_t *out)
{
    if (!context || !out) return;
    struct ff_dpdk_if_context *ctx = context;
    *out = ctx->stats;
}


static uint32_t ipv4_word(const uint8_t address[4])
{
    uint32_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

int port_nic_send_raw(void *context, const void *frame, size_t length)
{
    struct ff_dpdk_if_context *ctx = context;
    if (!ctx || !frame || !length || length > NIC_FRAME_BYTES)
        return EMSGSIZE;
    if (port_host_nic_send(ctx, frame, length) < 0) {
        ctx->stats.tx_errors++;
        return ENOBUFS;
    }
    ctx->stats.tx_frames++;
    return 0;
}

int port_nic_set_ipv6(void *context, const uint8_t address[16],
                              uint8_t prefix, int add)
{
    struct ff_dpdk_if_context *ctx = context;
    if (!ctx || !ctx->ifp || !address || prefix > 128)
        return EINVAL;
    struct in6_aliasreq ifra;
    memset(&ifra, 0, sizeof(ifra));
    strlcpy(ifra.ifra_name, ((struct ifnet *)ctx->ifp)->if_xname,
            sizeof(ifra.ifra_name));
    ifra.ifra_addr.sin6_family = AF_INET6;
    ifra.ifra_addr.sin6_len = sizeof(ifra.ifra_addr);
    memcpy(&ifra.ifra_addr.sin6_addr, address, 16);
    ifra.ifra_prefixmask.sin6_family = AF_INET6;
    ifra.ifra_prefixmask.sin6_len = sizeof(ifra.ifra_prefixmask);
    for (unsigned i = 0; i < 16; ++i) {
        unsigned bits = prefix > i * 8 ? prefix - i * 8 : 0;
        if (bits >= 8)
            ifra.ifra_prefixmask.sin6_addr.s6_addr[i] = 0xff;
        else if (bits)
            ifra.ifra_prefixmask.sin6_addr.s6_addr[i] =
                (uint8_t)(0xff << (8 - bits));
    }
    /* Infinite preferred/valid lifetime: the address is installed by an
     * administrator, not learned from RA. */
    ifra.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
    ifra.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;
    int error = in6_control(NULL,
                            add ? SIOCAIFADDR_IN6 : SIOCDIFADDR_IN6,
                            (caddr_t)&ifra, (struct ifnet *)ctx->ifp, NULL);
    return error ? error : 0;
}

static int route4(struct ifnet *ifp, uint32_t destination, uint8_t prefix,
                  uint32_t gateway, int add)
{
    struct rt_addrinfo info;
    struct rib_cmd_info rci;
    struct sockaddr_in gw, dst, mask;
    uint32_t mask_word = prefix == 0 ? 0 :
                         prefix >= 32 ? 0xffffffffU :
                         htonl(~((1u << (32 - prefix)) - 1));

    (void)ifp;
    bzero(&info, sizeof(info));
    bzero(&gw, sizeof(gw));
    bzero(&dst, sizeof(dst));
    bzero(&mask, sizeof(mask));
    gw.sin_len = dst.sin_len = mask.sin_len = sizeof(struct sockaddr_in);
    gw.sin_family = dst.sin_family = mask.sin_family = AF_INET;
    gw.sin_addr.s_addr = gateway;
    dst.sin_addr.s_addr = destination;
    mask.sin_addr.s_addr = mask_word;
    if (prefix == 32) info.rti_flags |= RTF_HOST;
    if (gateway) {
        info.rti_flags |= RTF_GATEWAY;
        info.rti_info[RTAX_GATEWAY] = (struct sockaddr *)&gw;
    }
    info.rti_info[RTAX_DST] = (struct sockaddr *)&dst;
    info.rti_info[RTAX_NETMASK] = (struct sockaddr *)&mask;
    return rib_action(RT_DEFAULT_FIB, add ? RTM_ADD : RTM_DELETE,
                      &info, &rci);
}

static int route_default(uint32_t gateway, int add)
{
    return route4(NULL, 0, 0, gateway, add);
}

static int v4_addr_ctl(struct ifnet *ifp, uint32_t address, uint32_t mask,
                       uint32_t broadcast, int add)
{
    struct socket *so = NULL;
    int error = socreate(AF_INET, &so, SOCK_DGRAM, 0,
                         curthread->td_ucred, curthread);
    if (error) return error;

    if (add) {
        struct in_aliasreq req;
        bzero(&req, sizeof(req));
        strlcpy(req.ifra_name, ifp->if_xname, IFNAMSIZ);
        req.ifra_addr.sin_len = sizeof(req.ifra_addr);
        req.ifra_addr.sin_family = AF_INET;
        req.ifra_addr.sin_addr.s_addr = address;
        req.ifra_mask.sin_len = sizeof(req.ifra_mask);
        req.ifra_mask.sin_family = AF_INET;
        req.ifra_mask.sin_addr.s_addr = mask;
        req.ifra_broadaddr.sin_len = sizeof(req.ifra_broadaddr);
        req.ifra_broadaddr.sin_family = AF_INET;
        req.ifra_broadaddr.sin_addr.s_addr = broadcast;
        error = ifioctl(so, SIOCAIFADDR, (caddr_t)&req, curthread);
    } else {
        struct ifreq ifr;
        bzero(&ifr, sizeof(ifr));
        strlcpy(ifr.ifr_name, ifp->if_xname, IFNAMSIZ);
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        sin->sin_len = sizeof(*sin);
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = address;
        error = ifioctl(so, SIOCDIFADDR, (caddr_t)&ifr, curthread);
    }
    soclose(so);
    return error;
}

int port_nic_ifindex(void *context)
{
    struct ff_dpdk_if_context *ctx = context;
    if (!ctx || !ctx->ifp) return -ENODEV;
    return (int)((struct ifnet *)ctx->ifp)->if_index;
}

int port_nic_set_mtu(void *context, int mtu)
{
    struct ff_dpdk_if_context *ctx = context;
    struct ifnet *ifp;
    if (!ctx || !ctx->ifp || mtu < 576 || mtu > NIC_FRAME_BYTES - 18)
        return EINVAL;
    ifp = ctx->ifp;
    return if_setmtu(ifp, mtu);
}

/*
 * Static ARP injection (Linux SIOCSARP / ATF_COM).
 *
 * dnsmasq answers a DHCP DISCOVER by unicasting the OFFER to the address it
 * just offered.  The client has not configured that address yet, so it cannot
 * answer an ARP request; dnsmasq therefore pre-loads the MAC with SIOCSARP.
 * Without this the OFFER sits in FreeBSD's ARP hold queue until it is dropped
 * and the lease never completes.
 *
 * The entry is inserted the same way arp_add_ifa_lle() does it, with
 * lltable_calc_llheader()/lltable_set_entry_addr() so the cached link header
 * (r_linkdata/r_hdrlen) is valid; arpresolve() copies that, not ll_addr.
 */
int port_nic_set_neighbor(void *context, uint32_t address, const uint8_t mac[6])
{
    struct ff_dpdk_if_context *ctx = context;
    struct ifnet *ifp;
    struct lltable *llt;
    struct llentry *lle, *old;
    struct sockaddr_in dst;
    u_char linkhdr[LLE_MAX_LINKHDR];
    size_t linkhdrsize = sizeof(linkhdr);
    int lladdr_off = 0;

    if (!ctx || !ctx->ifp || !mac) return EINVAL;
    ifp = ctx->ifp;
    if (ifp->if_addrlen != ETHER_ADDR_LEN) return EOPNOTSUPP;
    if (address == 0) return EINVAL;
    llt = LLTABLE(ifp);
    if (!llt) return EOPNOTSUPP;
    if (lltable_calc_llheader(ifp, AF_INET, (char *)mac, linkhdr,
                              &linkhdrsize, &lladdr_off) != 0) {
        printf("[NET] arp inject: calc_llheader failed on %s\n", ifp->if_xname);
        return EOPNOTSUPP;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_len = sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = address;

    lle = lltable_alloc_entry(llt, LLE_STATIC, (struct sockaddr *)&dst);
    if (!lle) {
        printf("[NET] arp inject: alloc failed on %s addr=%08x fib=%d\n",
               ifp->if_xname, address, ifp->if_fib);
        return ENOMEM;
    }

    IF_AFDATA_WLOCK(ifp);
    LLE_WLOCK(lle);
    old = lla_lookup(llt, LLE_EXCLUSIVE, (struct sockaddr *)&dst);
    if (old)
        lltable_unlink_entry(llt, old);
    lltable_set_entry_addr(ifp, lle, linkhdr, linkhdrsize, lladdr_off);
    lle->la_flags |= LLE_STATIC;
    lltable_link_entry(llt, lle);
    IF_AFDATA_WUNLOCK(ifp);
    EVENTHANDLER_INVOKE(lle_event, lle, LLENTRY_RESOLVED);
    LLE_WUNLOCK(lle);
    if (old)
        lltable_free_entry(llt, old);
    return 0;
}

void port_nic_set_forwarding(int ipv4, int ipv6)
{
    /* This port builds without VIMAGE, so the accessors are plain globals. */
    V_ipforwarding = ipv4 ? 1 : 0;
#ifdef INET6
    V_ip6_forwarding = ipv6 ? 1 : 0;
#else
    (void)ipv6;
#endif
    printf("[NET] BSD forwarding ipv4=%d ipv6=%d\n", ipv4, ipv6);
}

int port_nic_set_up(void *context, int up)
{
    struct ff_dpdk_if_context *ctx = context;
    struct ifnet *ifp;
    if (!ctx || !ctx->ifp) return ENODEV;
    ifp = ctx->ifp;
    if (up) if_up(ifp); else if_down(ifp);
    return 0;
}

int port_nic_route4(void *context, const uint8_t destination[4],
                    uint8_t prefix, const uint8_t gateway[4], int remove)
{
    struct ff_dpdk_if_context *ctx = context;
    if (!ctx || !ctx->ifp || !destination || prefix > 32) return EINVAL;
    return route4(ctx->ifp, ipv4_word(destination), prefix,
                  gateway ? ipv4_word(gateway) : 0, !remove);
}

int port_nic_set_ipv4(void *context,
                              const uint8_t address[4],
                              const uint8_t mask[4],
                              const uint8_t gateway[4],
                              const uint8_t old_address[4],
                              const uint8_t old_mask[4],
                              const uint8_t old_gateway[4])
{
    struct ff_dpdk_if_context *ctx = context;
    struct ifnet *ifp;
    uint32_t addr, netmask, gw = 0, old_gw = 0;
    if (!ctx || !ctx->ifp || !address || !mask) return EINVAL;
    ifp = ctx->ifp;
    addr = ipv4_word(address);
    netmask = ipv4_word(mask);
    if (gateway) gw = ipv4_word(gateway);
    if (old_gateway) old_gw = ipv4_word(old_gateway);
    uint32_t broadcast = addr | ~netmask;
    uint32_t prev_addr = old_address ? ipv4_word(old_address) : 0;
    uint32_t prev_mask = old_mask ? ipv4_word(old_mask) : 0;
    uint32_t prev_broadcast = prev_addr | ~prev_mask;

    int gateway_changed = gateway && gw != old_gw;
    if (addr == 0) {
        /* RTM_DELADDR passes an all-zero address.  Remove the configured
         * address (and its connected route) without installing 0.0.0.0. */
        if (prev_addr)
            (void)v4_addr_ctl(ifp, prev_addr, prev_mask, prev_broadcast, 0);
        if (gateway_changed && old_gw)
            (void)route_default(old_gw, 0);
        return 0;
    }
    if (gateway_changed && old_gw)
        (void)route_default(old_gw, 0);
    if (prev_addr)
        (void)v4_addr_ctl(ifp, prev_addr, prev_mask, prev_broadcast, 0);

    int error = v4_addr_ctl(ifp, addr, netmask, broadcast, 1);
    if (error) {
        if (prev_addr)
            (void)v4_addr_ctl(ifp, prev_addr, prev_mask, prev_broadcast, 1);
        if (gateway_changed && old_gw)
            (void)route_default(old_gw, 1);
        return error;
    }
    if (gw) {
        /* SIOCDIFADDR removes routes that depended on the old address.  The
         * default route is not part of the ifaddr transaction, so restore it
         * even when the gateway value itself did not change. */
        (void)route_default(gw, 0);
        error = route_default(gw, 1);
        if (error) {
            (void)v4_addr_ctl(ifp, addr, netmask, broadcast, 0);
            if (prev_addr)
                (void)v4_addr_ctl(ifp, prev_addr, prev_mask, prev_broadcast, 1);
            if (old_gw)
                (void)route_default(old_gw, 1);
            return error;
        }
    }
    return 0;
}
