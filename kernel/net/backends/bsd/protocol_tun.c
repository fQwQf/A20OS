/* Minimal FreeBSD L3 TUN interface for the A20OS /dev/net/tun service.
 *
 * F-Stack's if_tuntap depends on cdev/clone plumbing that this port does not
 * carry.  This file implements the part a TUN peer actually needs: an ifnet
 * that owns a connected subnet route, an outbound if_output that hands packets
 * to the A20OS TUN queue, and an inbound injector that enters the normal
 * FreeBSD IP netisr.
 * Address configuration still goes through in_control/in6_control, so the
 * interface behaves like any other BSD interface to routing and sockets.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/proc.h>
#include <sys/sockio.h>
#include <sys/errno.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/if_dl.h>
#include <net/route/nhop.h>
#include <net/route/route_ctl.h>
#include <net/netisr.h>
#include <sys/epoch.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>
#include "ff_host_interface.h"
#include "ff_veth.h"
#include <net/if_types.h>
#include "protocol_abi.h"

typedef struct port_tun {
    struct ifnet *ifp;
    a20_bsd_tun_emit_t emit;
    void *emit_ctx;
    uint32_t ip, mask, gw;
    int has_ip;
    int filter_index;
} port_tun_t;

typedef struct tun_reader_ctx {
    const uint8_t *data;
} tun_reader_ctx_t;

static size_t tun_reader(void *source, void *destination, size_t length)
{
    tun_reader_ctx_t *ctx = source;
    if (!ctx || !destination || !ctx->data)
        return 0;
    memcpy(destination, ctx->data, length);
    return length;
}

static int port_tun_output(struct ifnet *ifp, struct mbuf *m,
                           const struct sockaddr *dst, struct route *ro)
{
    (void)dst;
    (void)ro;
    port_tun_t *tun = ifp ? ifp->if_softc : NULL;
    if (!tun || !tun->emit || !m) {
        if (m) m_freem(m);
        return ENODEV;
    }
    int len = m->m_pkthdr.len;
    if (len <= 0 || len > 65535) {
        m_freem(m);
        return EMSGSIZE;
    }
    uint8_t *buffer = ff_calloc(1, (uint64_t)len);
    if (!buffer) {
        m_freem(m);
        return ENOBUFS;
    }
    int rc = ff_mbuf_copydata(m, buffer, 0, len);
    if (rc == 0) {
        tun_reader_ctx_t reader = {buffer};
        rc = tun->emit(tun->emit_ctx, (size_t)len, tun_reader, &reader);
    }
    ff_free(buffer);
    m_freem(m);
    return rc < 0 ? ENOBUFS : 0;
}

int port_tun_filter_index(const struct ifnet *ifp)
{
    if (!ifp || ifp->if_output != port_tun_output)
        return -1;
    const port_tun_t *tun = ifp->if_softc;
    return tun ? tun->filter_index : -1;
}

void *port_tun_create(int unit, int filter_index, a20_bsd_tun_emit_t emit, void *ctx)
{
    if (unit < 0 || unit > 1024 || !emit)
        return NULL;
    port_tun_t *tun = ff_calloc(1, sizeof(*tun));
    if (!tun)
        return NULL;
    tun->filter_index = filter_index;
    tun->emit = emit;
    tun->emit_ctx = ctx;
    struct ifnet *ifp = if_alloc(IFT_PPP);
    if (!ifp) {
        ff_free(tun);
        return NULL;
    }
    tun->ifp = ifp;
    ifp->if_softc = tun;
    if_initname(ifp, "tun", unit);
    ifp->if_mtu = 1500;
    ifp->if_flags = IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX;
    ifp->if_output = port_tun_output;
    if_attach(ifp);
    if_link_state_change(ifp, LINK_STATE_UP);
    return tun;
}

void port_tun_destroy(void *context)
{
    port_tun_t *tun = context;
    if (!tun) return;
    if (tun->ifp) {
        if_detach(tun->ifp);
        if_free(tun->ifp);
        tun->ifp = NULL;
    }
    ff_free(tun);
}

int port_tun_inject(void *context, const void *packet, size_t length)
{
    port_tun_t *tun = context;
    if (!tun || !tun->ifp || !packet || length < 20 || length > 65535)
        return EINVAL;
    struct mbuf *m = m_getm2(NULL, (int)length, M_NOWAIT, MT_DATA, M_PKTHDR);
    if (!m)
        return ENOBUFS;
    m_copyback(m, 0, (int)length, packet);
    m->m_pkthdr.len = (int)length;
    m->m_pkthdr.rcvif = tun->ifp;
    M_SETFIB(m, tun->ifp->if_fib);
    const uint8_t *p = packet;
    uint8_t version = p[0] >> 4;
    if (version != 4 && version != 6) {
        m_freem(m);
        return EAFNOSUPPORT;
    }
    struct epoch_tracker et;
    CURVNET_SET(tun->ifp->if_vnet);
    NET_EPOCH_ENTER(et);
    if (version == 4)
        netisr_dispatch(NETISR_IP, m);
    else
        netisr_dispatch(NETISR_IPV6, m);
    NET_EPOCH_EXIT(et);
    CURVNET_RESTORE();
    return 0;
}

int port_tun_set_mtu(void *context, uint16_t mtu)
{
    port_tun_t *tun = context;
    if (!tun || !tun->ifp || mtu < 68)
        return EINVAL;
    tun->ifp->if_mtu = mtu;
    return 0;
}

int port_tun_set_flags(void *context, uint32_t value, uint32_t mask)
{
    port_tun_t *tun = context;
    if (!tun || !tun->ifp)
        return ENODEV;
    uint32_t flags = tun->ifp->if_flags;
    uint32_t next = (flags & ~mask) | (value & mask);
    tun->ifp->if_flags = next;
    if ((next & IFF_UP) != (flags & IFF_UP))
        if_link_state_change(tun->ifp,
                             (next & IFF_UP) ? LINK_STATE_UP : LINK_STATE_DOWN);
    return 0;
}

int port_tun_get_addr(void *context, uint8_t address[4],
                              uint8_t mask[4], uint8_t gateway[4])
{
    port_tun_t *tun = context;
    if (!tun) return ENODEV;
    if (address) memcpy(address, &tun->ip, 4);
    if (mask) memcpy(mask, &tun->mask, 4);
    if (gateway) memcpy(gateway, &tun->gw, 4);
    return 0;
}

int port_tun_set_addr(void *context, const uint8_t address[4],
                      const uint8_t mask[4], int add)
{
    port_tun_t *tun = context;
    if (!tun || !tun->ifp || !address || !mask)
        return EINVAL;
    struct socket *so = NULL;
    int error = socreate(AF_INET, &so, SOCK_DGRAM, 0,
                         curthread->td_ucred, curthread);
    if (error) return error;
    if (add) {
        struct in_aliasreq req;
        memset(&req, 0, sizeof(req));
        strlcpy(req.ifra_name, tun->ifp->if_xname, sizeof(req.ifra_name));
        req.ifra_addr.sin_len = sizeof(req.ifra_addr);
        req.ifra_addr.sin_family = AF_INET;
        memcpy(&req.ifra_addr.sin_addr, address, 4);
        req.ifra_mask.sin_len = sizeof(req.ifra_mask);
        req.ifra_mask.sin_family = AF_INET;
        uint32_t a, m;
        memcpy(&a, address, 4);
        memcpy(&m, mask, 4);
        if (!tun->has_ip) {
            /* A Linux SIOCSIFADDR arrives before SIOCSIFNETMASK.  Installing
             * the address with an all-zero mask would add a 0.0.0.0/0
             * connected route on tun0 and shadow the real default route. */
            if (a == 0) {
                memcpy(&tun->mask, &m, 4);
                soclose(so);
                return 0;
            }
            if (m == 0) {
                uint8_t octet = ((const uint8_t *)address)[0];
                m = htonl(octet < 128 ? 0xff000000u :
                          octet < 192 ? 0xffff0000u : 0xffffff00u);
            }
        }
        memcpy(&req.ifra_mask.sin_addr, &m, 4);
        req.ifra_broadaddr.sin_len = sizeof(req.ifra_broadaddr);
        req.ifra_broadaddr.sin_family = AF_INET;
        req.ifra_broadaddr.sin_addr.s_addr = a | ~m;
        error = ifioctl(so, SIOCAIFADDR, (caddr_t)&req, curthread);
        if (error == 0) {
            memcpy(&tun->ip, address, 4);
            memcpy(&tun->mask, &m, 4);
            tun->has_ip = 1;
        }
    } else {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strlcpy(ifr.ifr_name, tun->ifp->if_xname, sizeof(ifr.ifr_name));
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        sin->sin_len = sizeof(*sin);
        sin->sin_family = AF_INET;
        memcpy(&sin->sin_addr, address, 4);
        error = ifioctl(so, SIOCDIFADDR, (caddr_t)&ifr, curthread);
        if (error == 0) {
            tun->ip = tun->mask = tun->gw = 0;
            tun->has_ip = 0;
        }
    }
    soclose(so);
    return error;
}

static int tun_route_match(const struct rtentry *rt, const struct nhop_object *nh,
                           void *context)
{
    (void)rt;
    return nh->nh_ifp == context;
}

/* An L3 link already knows where to send its packets. The RIB needs an
 * interface next hop and source address, with no IP gateway or ARP. */
int port_tun_route4(void *context, const uint8_t destination[4], uint8_t prefix,
                    int remove)
{
    port_tun_t *tun = context;
    if (!tun || !tun->ifp || !destination || prefix > 32)
        return EINVAL;
    if (!remove && !tun->has_ip)
        return EADDRNOTAVAIL;

    struct ifnet *ifp = tun->ifp;
    struct sockaddr_in dst = {.sin_len = sizeof(dst), .sin_family = AF_INET};
    struct sockaddr_in mask = {.sin_len = sizeof(mask), .sin_family = AF_INET};
    struct sockaddr_in local = {.sin_len = sizeof(local), .sin_family = AF_INET};
    memcpy(&dst.sin_addr, destination, 4);
    mask.sin_addr.s_addr = htonl(prefix ? (0xffffffffu << (32 - prefix)) : 0);
    local.sin_addr.s_addr = tun->ip;
    struct sockaddr_dl link = {
        .sdl_len = sizeof(link), .sdl_family = AF_LINK,
        .sdl_index = ifp->if_index, .sdl_type = ifp->if_type,
    };
    struct rt_addrinfo info = {0};
    info.rti_flags = RTF_STATIC | (prefix == 32 ? RTF_HOST : 0);
    info.rti_info[RTAX_DST] = (struct sockaddr *)&dst;
    info.rti_info[RTAX_NETMASK] = (struct sockaddr *)&mask;
    info.rti_info[RTAX_GATEWAY] = (struct sockaddr *)&link;
    info.rti_filter = tun_route_match;
    info.rti_filterdata = ifp;

    struct epoch_tracker et;
    struct rib_cmd_info result;
    NET_EPOCH_ENTER(et);
    if (!remove)
        info.rti_ifa = ifaof_ifpforaddr((struct sockaddr *)&local, ifp);
    int rc = !remove && !info.rti_ifa ? EADDRNOTAVAIL :
        rib_action(ifp->if_fib, remove ? RTM_DELETE : RTM_ADD, &info, &result);
    NET_EPOCH_EXIT(et);
    return rc;
}

int port_tun_set_addr6(void *context, const uint8_t address[16],
                               uint8_t prefix, int add)
{
    port_tun_t *tun = context;
    if (!tun || !tun->ifp || !address || prefix > 128)
        return EINVAL;
    struct in6_aliasreq ifra;
    memset(&ifra, 0, sizeof(ifra));
    strlcpy(ifra.ifra_name, tun->ifp->if_xname, sizeof(ifra.ifra_name));
    ifra.ifra_addr.sin6_family = AF_INET6;
    ifra.ifra_addr.sin6_len = sizeof(ifra.ifra_addr);
    memcpy(&ifra.ifra_addr.sin6_addr, address, 16);
    ifra.ifra_prefixmask.sin6_family = AF_INET6;
    ifra.ifra_prefixmask.sin6_len = sizeof(ifra.ifra_prefixmask);
    for (unsigned i = 0; i < 16; ++i) {
        unsigned bits = prefix > i * 8 ? prefix - i * 8 : 0;
        if (bits >= 8) ifra.ifra_prefixmask.sin6_addr.s6_addr[i] = 0xff;
        else if (bits) ifra.ifra_prefixmask.sin6_addr.s6_addr[i] =
            (uint8_t)(0xff << (8 - bits));
    }
    ifra.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
    ifra.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;
    return in6_control(NULL, add ? SIOCAIFADDR_IN6 : SIOCDIFADDR_IN6,
                       (caddr_t)&ifra, tun->ifp, NULL);
}

int port_tun_snapshot(void *context, a20_bsd_tun_info_t *out)
{
    port_tun_t *tun = context;
    if (!tun || !tun->ifp || !out)
        return EINVAL;
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, tun->ifp->if_xname, sizeof(out->name));
    out->mtu = tun->ifp->if_mtu;
    out->flags = (tun->ifp->if_flags & IFF_UP) ? 1u : 0u;
    if (tun->has_ip) {
        memcpy(out->ip, &tun->ip, 4);
        memcpy(out->mask, &tun->mask, 4);
        memcpy(out->gw, &tun->gw, 4);
        out->has_ip = 1;
    }
    return 0;
}

const char *port_tun_name(void *context)
{
    port_tun_t *tun = context;
    return (tun && tun->ifp) ? tun->ifp->if_xname : "";
}
