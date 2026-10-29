#include "tun_private.h"
#include "native.h"
#include "net/socket/socket_internal.h"
#include "net/lwip_stack.h"
#include "net/tun.h"
#include "core/string.h"
#include "lwip/igmp.h"

#ifdef CONFIG_LWIP_UPSTREAM
int tcp_cong_alg_parse(const char *name) { return strcmp(name, "reno") ? -1 : 0; }
const char *tcp_cong_alg_name(uint8_t algorithm) {
    (void)algorithm;
    return "reno";
}
#endif

uint32_t a20_lwip_transport_buffer_default(int send) {
#ifdef CONFIG_LWIP_UPSTREAM
    return send ? TCP_SND_BUF : TCP_WND;
#else
    return send && NET_PROFILE_TCP_BUF_INIT < TCP_SND_BUF
        ? NET_PROFILE_TCP_BUF_INIT : send ? TCP_SND_BUF : TCP_WND;
#endif
}

int a20_lwip_transport_option_validate(net_socket_t *s, int level, int option,
                              const void *value, size_t length) {
#ifdef CONFIG_LWIP_UPSTREAM
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        s->type == SOCK_STREAM && level == SOL_SOCKET &&
        (option == SO_SNDBUF || option == SO_RCVBUF)) {
        if (!value || length < sizeof(int)) return -EINVAL;
        int requested;
        memcpy(&requested, value, sizeof(requested));
        /* Upstream reopens windows and send space against compile-time limits.
         * Claiming a smaller per-PCB limit would require another core fork. */
        if (requested < (int)(option == SO_SNDBUF ? TCP_SND_BUF : TCP_WND))
            return -EOPNOTSUPP;
    }
#else
    (void)s; (void)level; (void)option; (void)value; (void)length;
#endif
    return 0;
}

typedef struct net_tcp_options_snapshot {
    uint8_t congestion;
    uint8_t nodelay;
    uint8_t keepalive;
    uint8_t reuseaddr;
    int keep_idle;
    int keep_intvl;
    int keep_cnt;
    uint32_t snd_buf;
    uint32_t rcv_buf;
    uint32_t generation;
} net_tcp_options_snapshot_t;


/* Never hold the socket lock with the lwIP control barrier.  If another
 * setter publishes while this snapshot waits for the barrier, discard it and
 * retry the complete option group so independent fields cannot be lost. */
enum {
    NET_TCP_APPLY_CONGESTION = 1u << 0,
    NET_TCP_APPLY_NODELAY    = 1u << 1,
    NET_TCP_APPLY_KEEPALIVE  = 1u << 2,
    NET_TCP_APPLY_REUSEADDR  = 1u << 3,
    NET_TCP_APPLY_KEEP_IDLE  = 1u << 4,
    NET_TCP_APPLY_KEEP_INTVL = 1u << 5,
    NET_TCP_APPLY_KEEP_CNT   = 1u << 6,
    NET_TCP_APPLY_SNDBUF     = 1u << 7,
    NET_TCP_APPLY_RCVBUF     = 1u << 8,
};

void a20_lwip_transport_options_apply(net_socket_t *s, unsigned mask)
{
    if (!lw(s)) return; /* UNIX/ALG sockets have no transport attachment. */
    for (;;) {
        net_tcp_options_snapshot_t opts;
        uint64_t sf = net_sock_lock(s);
        opts.congestion = s->tcp_congestion;
        opts.nodelay = s->tcp_nodelay;
        opts.keepalive = s->keepalive;
        opts.reuseaddr = s->reuseaddr;
        opts.keep_idle = s->keep_idle;
        opts.keep_intvl = s->keep_intvl;
        opts.keep_cnt = s->keep_cnt;
        opts.snd_buf = s->snd_buf;
        opts.rcv_buf = s->rcv_buf;
        opts.generation = __atomic_load_n(&s->tcp_opts_generation,
                                          __ATOMIC_RELAXED);
        net_sock_unlock(s, sf);

        uint64_t flags = a20_lwip_lock();
        if (__atomic_load_n(&s->tcp_opts_generation, __ATOMIC_ACQUIRE) !=
            opts.generation) {
            a20_lwip_unlock(flags);
            continue;
        }
        if (lw(s)->tcp) {
            if (mask & NET_TCP_APPLY_KEEPALIVE) {
                if (opts.keepalive)
                    lw(s)->tcp->so_options |= SOF_KEEPALIVE;
                else
                    lw(s)->tcp->so_options &= ~SOF_KEEPALIVE;
            }
            if (mask & NET_TCP_APPLY_REUSEADDR) {
                if (opts.reuseaddr)
                    ip_set_option(lw(s)->tcp, SOF_REUSEADDR);
                else
                    ip_reset_option(lw(s)->tcp, SOF_REUSEADDR);
            }
            /* LISTEN uses tcp_pcb_listen, which only has the common PCB
             * prefix.  Its child options live in net_socket_t until accept
             * creates a full tcp_pcb. */
            if (lw(s)->tcp->state != LISTEN) {
                if (mask & NET_TCP_APPLY_CONGESTION)
                    a20_net_cong_apply(lw(s)->tcp, opts.congestion);
                if (mask & NET_TCP_APPLY_NODELAY) {
                    if (opts.nodelay)
                        tcp_nagle_disable(lw(s)->tcp);
                    else
                        tcp_nagle_enable(lw(s)->tcp);
                }
                if ((mask & NET_TCP_APPLY_KEEP_IDLE) && opts.keep_idle > 0)
                    lw(s)->tcp->keep_idle = (u32_t)opts.keep_idle * 1000U;
                if ((mask & NET_TCP_APPLY_KEEP_INTVL) && opts.keep_intvl > 0)
                    lw(s)->tcp->keep_intvl = (u32_t)opts.keep_intvl * 1000U;
                if ((mask & NET_TCP_APPLY_KEEP_CNT) && opts.keep_cnt > 0)
                    lw(s)->tcp->keep_cnt = (u32_t)opts.keep_cnt;
                net_inet_tcp_buf_apply(
                    (mask & NET_TCP_APPLY_SNDBUF) ? opts.snd_buf : 0,
                    (mask & NET_TCP_APPLY_RCVBUF) ? opts.rcv_buf : 0,
                    lw(s)->tcp);
            }
        }
        a20_lwip_unlock(flags);
        return;
    }
}

/* Linux UABI, the value MCAST_JOIN_GROUP / MCAST_LEAVE_GROUP carry across the
 * syscall boundary:
 *
 *   struct ip_mreqn { __be32 imr_multiaddr; __be32 imr_address; int imr_ifindex; }
 *
 * All three members are exactly 4 bytes, so the layout is frozen.  There is no
 * family member: Linux derives it from the socket, which is why an AF_INET6
 * socket asking at IPPROTO_IP is refused below instead of being joined.  This
 * belongs next to net_sockaddr_in_t in kernel/include/net/socket.h, which is
 * owned by the integrator; until then it is declared here so the wire layout
 * has exactly one definition in the tree. */
typedef struct net_ip_mreqn {
    uint32_t imr_multiaddr;   /* group address, network byte order */
    uint32_t imr_address;     /* local interface address, network order */
    int32_t  imr_ifindex;     /* interface index; 0 selects the default netif */
} net_ip_mreqn_t;

/*
 * Linux UABI wire values for the two multicast options.  kernel/include/net/
 * socket.h spells them 10 and 11, but those are the BSD IP_PMTUDISC /
 * IP_RECVERR numbers: Linux puts IP_MULTICAST_TTL at 33 and IP_MULTICAST_LOOP
 * at 34, which is what user/external/musl/include/netinet/in.h:209-210 hands
 * to unmodified musl programs.  Dispatching on the header's values would
 * refuse every such call, so the wire numbers are spelled out here and the
 * header needs the same correction.  MCAST_JOIN_GROUP (42) and
 * MCAST_LEAVE_GROUP (45) in that header are already right.
 */
#define A20_IP_MULTICAST_TTL_WIRE  33
#define A20_IP_MULTICAST_LOOP_WIRE 34

/*
 * Read one of the small integer IPPROTO_IP options, in either byte order.
 * Linux stores IP_TTL, IP_TOS and IP_MULTICAST_* in network byte order, so a
 * caller following the ABI literally passes htonl(v) -- and on every
 * little-endian target this kernel builds, a plain host-order v is the byte
 * swap of that.  Accepting both encodings is deliberate: rejecting the
 * readable 64 would break the common case, and rejecting htonl(64) would
 * break the network-aware one.  A value that is out of range in both orders is
 * still refused, so this widens the accepted set without accepting junk.
 * Returns the value, or a negative errno.
 */

/*
 * Resolve the interface a multicast membership change names.  Linux prefers
 * imr_ifindex and falls back to imr_address; both zero means "whatever the
 * default route uses".  A selector that matches nothing returns NULL so the
 * caller fails, instead of joining on an interface the caller never named.
 * Must be called with g_lwip_lock held: netif_list is lwIP state.
 */
static struct netif *net_ip_group_netif(const net_ip_mreqn_t *mreq)
{
    if (mreq->imr_ifindex > 0) {
        for (struct netif *n = netif_list; n; n = n->next)
            if ((int32_t)netif_get_index(n) == mreq->imr_ifindex)
                return n;
        return NULL;
    }
    if (mreq->imr_address) {
        for (struct netif *n = netif_list; n; n = n->next)
            if ((n->state || a20_lwip_netif_is_tun(n)) &&
                netif_ip4_addr(n)->addr == mreq->imr_address)
                return n;
        return NULL;
    }
    return netif_default;
}

/*
 * MCAST_JOIN_GROUP / MCAST_LEAVE_GROUP.  The membership is host-wide IGMP
 * state rather than socket state -- that is what the Linux ABI means by it --
 * so nothing is stored on net_socket_t and the socket's implicit membership
 * from bind()/connect() is deliberately left alone.
 */
static int net_ip_group_membership(net_socket_t *s, const net_ip_mreqn_t *mreq,
                                   int join)
{
    if (s->domain != AF_INET)
        return -EAFNOSUPPORT;
    ip4_addr_t group;
    group.addr = mreq->imr_multiaddr;
    if (!ip4_addr_ismulticast(&group))
        return -EINVAL;

    /* g_lwip_lock alone: the netif list and the IGMP group table are lwIP
     * state, and the two kernel locks must never be held together (see
     * docs/net/network-lock-contract.md).  Nothing here allocates. */
    uint64_t flags = a20_lwip_lock();
    struct netif *nif = net_ip_group_netif(mreq);
    if (!nif) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    if (!netif_is_up(nif)) {
        /* igmp_joingroup() would still create the group and emit a report from
         * an interface with no carrier, i.e. a membership nobody can use. */
        a20_lwip_unlock(flags);
        return -ENETDOWN;
    }
    if (ip4_addr_isany(netif_ip4_addr(nif))) {
        /* An interface with no IPv4 address cannot source the report, and
         * igmp_joingroup() reads 0.0.0.0 as "join on every interface". */
        a20_lwip_unlock(flags);
        return -EADDRNOTAVAIL;
    }
    if (!(nif->flags & NETIF_FLAG_IGMP)) {
        /* No multicast filter on the driver, so the group could never be
         * delivered.  Say so instead of joining and reporting success. */
        a20_lwip_unlock(flags);
        return -EOPNOTSUPP;
    }
    err_t e = join ? igmp_joingroup(netif_ip4_addr(nif), &group)
                   : igmp_leavegroup(netif_ip4_addr(nif), &group);
    a20_lwip_unlock(flags);
    if (e == ERR_OK)
        return 0;
    if (e == ERR_MEM)
        return -ENOMEM;
    /* lwIP returns ERR_VAL for the all-systems group in both directions, and
     * for a leave of a group this host never joined. */
    return join ? -EINVAL : -ENOENT;
}


int a20_net_option_group_membership(void *sp, const void *mreq, int join)
{
    return net_ip_group_membership((net_socket_t *)sp,
                                   (const net_ip_mreqn_t *)mreq, join);
}


int a20_net_option_tcp_live(void *sp, int which)
{
    net_socket_t *s = (net_socket_t *)sp;
    if (!lw(s)) return -1;
    if (which == 0) {
#if LWIP_TCP_CUBIC
        uint64_t flags = a20_lwip_lock();
        int alg = (lw(s)->tcp && lw(s)->tcp->state != LISTEN)
                      ? (int)lw(s)->tcp->cong_alg : -1;
        a20_lwip_unlock(flags);
        return alg;
#else
        return TCP_CONG_RENO;
#endif
    }
    uint64_t flags = a20_lwip_lock();
    int v = -1;
    if (lw(s)->tcp && lw(s)->tcp->state != LISTEN) {
        switch (which) {
        case 1: v = (int)(lw(s)->tcp->keep_idle / 1000U); break;
        case 2: v = (int)(lw(s)->tcp->keep_intvl / 1000U); break;
        case 3: v = (int)lw(s)->tcp->keep_cnt; break;
        default: break;
        }
    }
    a20_lwip_unlock(flags);
    return v;
}

/* The TTL/TOS/multicast-TTL a packet would actually carry.  For an option the
 * caller never set that is the pcb's value, not the stored zero. */
