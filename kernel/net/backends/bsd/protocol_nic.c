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
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/socketvar.h>
#include <sys/proc.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/route/route_ctl.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>
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
    void *softc;
    void *ifp;
    int port_id;
    nic_token_t tokens[NIC_RX_TOKENS];
    nic_token_t *free_tokens;
    unsigned rx_outstanding;
    uint8_t tx_frame[NIC_FRAME_BYTES];
    a20_bsd_nic_stats_t stats;
};

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
    return ctx;
}

void ff_dpdk_deregister_if(struct ff_dpdk_if_context *ctx)
{
    if (!ctx) return;
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
    } else if (a20_bsd_netfilter_output(ctx->tx_frame, (size_t)total,
                                        ctx->port_id) == BSD_NETFILTER_DROP) {
        /* Policy drop is a successful consumption, not a driver error. */
        result = 0;
        ctx->stats.tx_drops++;
        ff_mbuf_free(m);
        return result;
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
        if (a20_bsd_netfilter_input(token->frame, (size_t)length,
                                    ctx->port_id) == BSD_NETFILTER_DROP) {
            ctx->stats.rx_drops++;
            token_return(token, 0);
            continue;
        }
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
