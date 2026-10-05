#include "net/socket_internal.h"
#include "ipc/ipc.h"
#include "core/klog.h"
#include "core/string.h"
#include "net/lwip_stack.h"
#include "net/net_config.h"
#include "lwip/tcp.h"
#include "lwip/igmp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip.h"
/* TCP_CONGESTION name validation and the real algorithm name.  A20OS divergence
 * in lwIP; see kernel/external/lwip/DIVERGENCE.md 2.7.  Included
 * unconditionally: the TCP_CONG_* values are outside the header's
 * LWIP_TCP_CUBIC guard precisely so that a build without CUBIC can still accept
 * "reno" and reject "cubic" by name. */
#include "lwip/priv/tcp_cubic_priv.h"

#ifndef SHUT_RD
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
#endif

/* Linux errno 100.  kernel/include/core/errno.h stops at EADDRNOTAVAIL (99)
 * and resumes at ENETUNREACH (101), so the value a caller reads back for
 * "interface is down" is spelled out here; core/errno.h needs the same line. */
#ifndef ENETDOWN
#define ENETDOWN 100
#endif

static uint64_t timeval_to_ticks(const void *optval, size_t optlen)
{
    if (!optval || optlen < sizeof(long) * 2)
        return 0;
    const long *tv = (const long *)optval;
    if (tv[0] < 0 || tv[1] < 0)
        return 0;
    uint64_t ticks = (uint64_t)tv[0] * TICKS_PER_SEC +
                     (uint64_t)tv[1] * TICKS_PER_SEC / 1000000ULL;
    return ticks ? ticks : 1;
}

static int net_copyout_int(void *optval, size_t *optlen, int val)
{
    if (!optval || !optlen || *optlen < sizeof(int))
        return -EINVAL;
    memcpy(optval, &val, sizeof(val));
    *optlen = sizeof(val);
    return 0;
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
static int net_ipopt_byte(const void *optval, size_t optlen, int lo, int hi)
{
    if (!optval || optlen < sizeof(int))
        return -EINVAL;
    int val;
    memcpy(&val, optval, sizeof(val));
    if (val < 0 || val > 0xff) {
        uint32_t raw = (uint32_t)val;
        val = (int)(((raw & 0xff000000u) >> 24) | ((raw & 0x00ff0000u) >> 8) |
                    ((raw & 0x0000ff00u) << 8) | ((raw & 0x000000ffu) << 24));
    }
    return (val >= lo && val <= hi) ? val : -EINVAL;
}

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
            if (n->state && netif_ip4_addr(n)->addr == mreq->imr_address)
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

int net_listen(int gfd, int backlog)
{
    return net_listen_sock(net_socket_from_file(gfd), backlog);
}

int net_listen_sock(net_socket_t *s, int backlog)
{
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG)
        return 0;
    if ((s->type != SOCK_STREAM && !(s->domain == AF_UNIX && s->type == SOCK_SEQPACKET)) ||
        (s->domain != AF_INET && s->domain != AF_INET6 && s->domain != AF_UNIX))
        return -EOPNOTSUPP;
    if (s->domain == AF_UNIX && !s->bound)
        return -EINVAL;
    if (s->listening)
        return 0;
    if (backlog <= 0)
        backlog = 1;
    if (backlog > NET_MAX_QUEUE)
        backlog = NET_MAX_QUEUE;

    /*
     * Two ways to hold a listening socket, selected by tcpmode.
     *
     * fast (default): the listener never exists in lwIP.  It sets local_tcp and
     * drops the bound pcb, so a connection is only ever matched by another
     * process in this same kernel taking the same shortcut.  Kept as the
     * default because it is what the existing accept tests were written
     * against, and the comment that used to sit here recorded why: LTP's
     * localhost accept tests exercise close-after-accept heavily, and a
     * listener kept as a plain bound pcb gives deterministic close semantics.
     *
     * lwip: convert the bound pcb into a real LISTEN pcb, so the port is
     * actually listening in the protocol stack and an inbound connection from
     * off-box completes its handshake.  The accept queue, the child
     * net_socket_t and the wakeup are the socket layer's in both modes, so this
     * changes reachability only.
     *
     * Both families take the lwip path.  It was AF_INET-only, which combined
     * with net_inet_socket_init's AF_INET-only pcb arm to make AF_INET6
     * inbound TCP impossible in either mode: the fast path dropped a NULL pcb
     * and left the port unreachable, and the lwip path refused outright.
     */
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        g_a20_tcp_path == A20_TCP_PATH_LWIP) {
        int r = net_inet_tcp_listen(s, backlog);
        if (r < 0) {
            s->listening = 0;
            return r;
        }
        s->listening = 1;
        s->local_tcp = 0;
    } else {
        s->listening = 1;
        if (s->domain == AF_INET || s->domain == AF_INET6) {
            s->local_tcp = 1;
            if (s->tcp)
                net_tcp_drop_pcb(s);
        }
    }
    if (s->domain == AF_INET || s->domain == AF_INET6) {
        uint16_t lport = 0;
        net_sockaddr_port(s->local, s->local_len, &lport);
        ktrace_net("[NET] listen port=%u mode=%s\n", (unsigned)net_ntohs(lport),
                   s->local_tcp ? "fast" : "lwip");
    }
    return 0;
}

int net_accept(int gfd, void *addr, size_t *addrlen, int flags)
{
    return net_accept_sock(net_socket_from_file(gfd), addr, addrlen, flags);
}

int net_accept_sock(net_socket_t *s, void *addr, size_t *addrlen, int flags)
{
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
        return -EINVAL;
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG)
        return net_alg_socket_accept(s, addrlen, flags);
    if ((s->type != SOCK_STREAM && !(s->domain == AF_UNIX && s->type == SOCK_SEQPACKET)) ||
        (s->domain != AF_INET && s->domain != AF_INET6 && s->domain != AF_UNIX))
        return -EOPNOTSUPP;
    if (!s->listening)
        return -EINVAL;

    net_socket_t *child = NULL;
    uint64_t start = timer_get_ticks();
    int sb = net_socket_bucket(s);
    for (;;) {
        uint64_t irq = net_bucket_lock(sb);
        if (s->closed) {
            net_bucket_unlock(sb, irq);
            return -EINVAL;
        }
        child = net_accept_queue_pop_locked(s);
        if (child) {
            ktrace_net("[NET] accept: popped child from queue\n");
            net_bucket_unlock(sb, irq);
            break;
        }
        if (s->nonblock) {
            net_bucket_unlock(sb, irq);
            return -EAGAIN;
        }
        task_t *cur = proc_current();
        if (!cur) {
            net_bucket_unlock(sb, irq);
            return -EAGAIN;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_bucket_unlock(sb, irq);
            return -ERESTARTSYS;
        }
        if (net_socket_wait_expired(s, start, 0)) {
            net_bucket_unlock(sb, irq);
            return -EAGAIN;
        }
        uint64_t deadline = s->recv_timeout_ticks ?
                            start + s->recv_timeout_ticks : 0;
        net_bucket_unlock(sb, irq);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        irq = net_bucket_lock(sb);
        if (s->closed || s->accept_head) {
            net_bucket_unlock(sb, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_bucket_unlock(sb, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ERESTARTSYS;
        }
        bool linked =
            wait_queue_link(&s->accept_waitq, &entry, token, 0);
        net_bucket_unlock(sb, irq);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&s->accept_waitq, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason) ||
            net_task_has_unblocked_signal(cur))
            return -ERESTARTSYS;
        if (reason == PROC_WAKE_TIMEOUT)
            return -EAGAIN;
    }

    net_inet_accept_child_ready(child);

    if (addr && addrlen && *addrlen > 0) {
        size_t n = child->peer_len < *addrlen ? child->peer_len : *addrlen;
        memcpy(addr, child->peer_addr, n);
        *addrlen = n;
    }

    child->nonblock = (flags & SOCK_NONBLOCK) ? 1 : s->nonblock;
    ktrace_net("[NET] accept: done, installing file\n");
    int newfd = net_socket_install_file(child, O_RDWR | (child->nonblock ? O_NONBLOCK : 0));
    if (newfd < 0) {
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        net_socket_t *peer = NULL;
        bool drain_peer_read = false;
        bool drain_peer_write = false;
        /* child and its peer, two buckets, ascending.  child is pinned -- it is
         * the socket accept() just popped off the listener's queue, carrying its
         * creator's reference -- but child->peer is a plain back-pointer with no
         * reference of its own, so it is sampled and referenced under the
         * child's bucket before the pair is taken. */
        int cb = net_socket_bucket(child);
        {
            uint64_t cf = net_bucket_lock(cb);
            if (child->peer)
                peer = net_socket_ref(child->peer);
            net_bucket_unlock(cb, cf);
        }
        net_bucket_pair_t pair = net_bucket_lock2(cb, net_socket_bucket(peer));
        child->closed = 1;
        if (peer && peer->peer == child) {
            peer->peer = NULL;
            peer->peer_closed = 1;
            drain_peer_read = net_wait_queue_collect_all_locked(
                &peer->read_waitq, PROC_WAKE_EVENT, &wake_q);
            drain_peer_write = net_wait_queue_collect_all_locked(
                &peer->write_waitq, PROC_WAKE_EVENT, &wake_q);
        }
        net_unregister_socket_locked(child);
        net_bucket_unlock2(pair);
        /* Registry reference, dropped outside the lock: it can free. */
        net_socket_free(child);
        (void)proc_wake_q_flush(&wake_q);
        if (drain_peer_read)
            (void)wait_queue_wake_all(
                &peer->read_waitq, 0, PROC_WAKE_EVENT);
        if (drain_peer_write)
            (void)wait_queue_wake_all(
                &peer->write_waitq, 0, PROC_WAKE_EVENT);
        net_inet_socket_destroy(child);
        net_socket_free(child);
        if (peer)
            net_socket_free(peer);
        return newfd;
    }
    return newfd;
}

int net_getsockname(int gfd, void *addr, size_t *addrlen)
{
    return net_getsockname_sock(net_socket_from_file(gfd), addr, addrlen);
}

int net_getsockname_sock(net_socket_t *s, void *addr, size_t *addrlen)
{
    if (!s)
        return -ENOTSOCK;
    if (!addr || !addrlen)
        return -EFAULT;
    int b = net_socket_bucket(s);
    uint64_t irq = net_bucket_lock(b);
    if (!net_socket_is_live(s)) {
        net_bucket_unlock(b, irq);
        return -ENOTSOCK;
    }
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    size_t n = s->local_len < *addrlen ? s->local_len : *addrlen;
    memcpy(addr, s->local, n);
    *addrlen = n;
    net_bucket_unlock(b, irq);
    return 0;
}

int net_getpeername(int gfd, void *addr, size_t *addrlen)
{
    return net_getpeername_sock(net_socket_from_file(gfd), addr, addrlen);
}

int net_getpeername_sock(net_socket_t *s, void *addr, size_t *addrlen)
{
    if (!s)
        return -ENOTSOCK;
    if (!s->connected)
        return -ENOTCONN;
    size_t n = s->peer_len < *addrlen ? s->peer_len : *addrlen;
    memcpy(addr, s->peer_addr, n);
    *addrlen = n;
    return 0;
}

int net_setsockopt(int gfd, int level, int optname, const void *optval, size_t optlen)
{
    return net_setsockopt_sock(net_socket_from_file(gfd), level, optname,
                               optval, optlen);
}

int net_setsockopt_sock(net_socket_t *s, int level, int optname,
                        const void *optval, size_t optlen)
{
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG && level == SOL_ALG && optname == ALG_SET_KEY) {
        /* No provider, so there is no key schedule to install.  Returning 0
         * here would let a caller believe a key was accepted; see
         * kernel/net/socket_alg.c. */
        return -EOPNOTSUPP;
    }
    if (level == IPPROTO_IP) {
        if (optname == MCAST_JOIN_GROUP || optname == MCAST_LEAVE_GROUP ||
            optname == IP_ADD_MEMBERSHIP || optname == IP_DROP_MEMBERSHIP) {
            if (!optval || optlen < sizeof(net_ip_mreqn_t))
                return -EINVAL;
            net_ip_mreqn_t mreq;
            memcpy(&mreq, optval, sizeof(mreq));
            return net_ip_group_membership(s, &mreq,
                                          optname == MCAST_JOIN_GROUP ||
                                          optname == IP_ADD_MEMBERSHIP);
        }
        if (optname == A20_IP_MULTICAST_TTL_WIRE ||
            optname == A20_IP_MULTICAST_LOOP_WIRE) {
            /* Both only mean something for a pcb that can emit a multicast
             * datagram, and a stream socket never has one, so storing the
             * value would be a silent no-op. */
            if (s->type == SOCK_STREAM)
                return -EINVAL;
        }
        if (optname == A20_IP_MULTICAST_TTL_WIRE) {
            int val = net_ipopt_byte(optval, optlen, 0, 0xff);
            if (val < 0)
                return val;
            s->mc_ttl = (uint8_t)val;
            net_inet_ip_opts_apply(s);
            return 0;
        }
        if (optname == A20_IP_MULTICAST_LOOP_WIRE) {
            int val = net_ipopt_byte(optval, optlen, 0, 1);
            if (val < 0)
                return val;
            s->mc_loop = (uint8_t)val;
            net_inet_ip_opts_apply(s);
            return 0;
        }
        if (optname == IP_TTL) {
            int val = net_ipopt_byte(optval, optlen, 1, 0xff);
            if (val < 0)
                return val;
            s->ip_ttl = (uint8_t)val;
            s->ip_ttl_set = 1;
            net_inet_ip_opts_apply(s);
            return 0;
        }
        if (optname == IP_TOS) {
            int val = net_ipopt_byte(optval, optlen, 0, 0xff);
            if (val < 0)
                return val;
            s->ip_tos = (uint8_t)val;
            s->ip_tos_set = 1;
            net_inet_ip_opts_apply(s);
            return 0;
        }
        /* IP_OPTIONS is the interesting refusal: honouring it needs a
         * per-packet option buffer and lwIP's raw/udp/tcp output takes none
         * (raw_sendto_if, udp_sendto_if_src, tcp_output), so the options
         * could only be written to the IP header by a second send path.
         * IP_HDRINCL, IP_TRANSPARENT and the rest are likewise unimplemented.
         * Policy: a setsockopt a caller cannot honour must fail, never
         * silently succeed. */
        return -EOPNOTSUPP;
    }
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_CHECKSUM) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int offset;
        memcpy(&offset, optval, sizeof(offset));
        if (offset >= 0 && (offset & 1))
            return -EINVAL;
        s->ipv6_checksum_offset = offset;
        return 0;
    }
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_V6ONLY) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->ipv6_v6only = val != 0;
        return 0;
    }
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 &&
        (optname == IPV6_RECVPKTINFO || optname == IPV6_RECVTCLASS ||
         optname == IPV6_RECVHOPLIMIT || optname == IPV6_RECVRTHDR ||
         optname == IPV6_RECVHOPOPTS || optname == IPV6_RECVDSTOPTS ||
         optname == IPV6_RECVERR || optname == IPV6_2292PKTINFO ||
         optname == IPV6_2292HOPLIMIT || optname == IPV6_2292RTHDR ||
         optname == IPV6_2292HOPOPTS || optname == IPV6_2292DSTOPTS)) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        if (optname == IPV6_RECVPKTINFO)  s->ipv6_recv_pktinfo = val != 0;
        if (optname == IPV6_RECVTCLASS)   s->ipv6_recv_tclass  = val != 0;
        if (optname == IPV6_RECVHOPLIMIT) s->ipv6_recv_hoplimit = val != 0;
        if (optname == IPV6_RECVRTHDR)    s->ipv6_recv_rthdr   = val != 0;
        if (optname == IPV6_RECVHOPOPTS)  s->ipv6_recv_hopopts = val != 0;
        if (optname == IPV6_RECVDSTOPTS)  s->ipv6_recv_dstopts = val != 0;
        if (optname == IPV6_RECVERR)      s->ipv6_recv_err     = val != 0;
        if (optname == IPV6_2292PKTINFO)  s->ipv6_recv_2292_pktinfo = val != 0;
        if (optname == IPV6_2292HOPLIMIT) s->ipv6_recv_2292_hoplimit = val != 0;
        if (optname == IPV6_2292RTHDR)    s->ipv6_recv_2292_rthdr = val != 0;
        if (optname == IPV6_2292HOPOPTS)  s->ipv6_recv_2292_hopopts = val != 0;
        if (optname == IPV6_2292DSTOPTS)  s->ipv6_recv_2292_dstopts = val != 0;
        return 0;
    }
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 &&
        (optname == IPV6_UNICAST_IF || optname == IPV6_MULTICAST_IF ||
         optname == IPV6_MULTICAST_HOPS || optname == IPV6_MULTICAST_LOOP ||
         optname == IPV6_TCLASS || optname == IPV6_HOPLIMIT ||
         optname == IPV6_FLOWINFO || optname == IPV6_ROUTER_ALERT))
        return optlen >= sizeof(int) ? 0 : -EINVAL;
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 &&
        (optname == IPV6_JOIN_GROUP || optname == IPV6_LEAVE_GROUP))
        return 0;
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_ADDRFORM) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        if (val != AF_INET)
            return -EINVAL;
        if (s->type != SOCK_STREAM || s->connected || s->bound)
            return -EOPNOTSUPP;
        s->domain = AF_INET;
        return 0;
    }
    if (s->domain == AF_INET6 && s->type == SOCK_RAW &&
        level == IPPROTO_ICMPV6 && optname == ICMP6_FILTER) {
        if (!optval || optlen < sizeof(s->icmp6_filter))
            return -EINVAL;
        memcpy(s->icmp6_filter, optval, sizeof(s->icmp6_filter));
        s->icmp6_filter_set = 1;
        return 0;
    }
    if (level == IPPROTO_TCP) {
        if (s->type != SOCK_STREAM)
            return -ENOPROTOOPT;
        if (optname == TCP_CONGESTION) {
            /* Validate the name instead of accepting anything.  This used to
             * `return optval && optlen ? 0 : -EINVAL`, so "bbr", "reno " and
             * "cubic-but-not-really" all set successfully on a stack that has
             * exactly one algorithm, and the caller had no way to find out
             * which one it got.  -ENOPROTOOPT is what Linux returns for an
             * algorithm it cannot honour, and it is the only answer that tells
             * the truth: the request was well-formed, this kernel has no such
             * algorithm.  That includes the EMBEDDED profile, where CUBIC is
             * compiled out -- "cubic" is a real name this build cannot serve. */
            char name[16];
            size_t n;
            int alg;
            if (!optval || !optlen)
                return -EINVAL;
            /* The name is a NUL-terminated string, not a fixed-size blob: Linux
             * treats optlen as the buffer size and reads up to it.  Truncating
             * at 15 chars means an over-long name is rejected as unknown
             * rather than silently matching a prefix. */
            n = optlen < sizeof(name) ? optlen : sizeof(name);
            memcpy(name, optval, n);
            name[n - 1] = '\0';
            alg = tcp_cong_alg_parse(name);
            if (alg < 0)
                return -ENOPROTOOPT;
            s->tcp_congestion = (uint8_t)alg;
            /* Apply immediately to a live pcb as well as to future ones, so
             * the option means what the caller just asked for.  Linux allows
             * this on an established connection.  Switching does not reset the
             * window or discard the algorithm's state: RFC 8312 4.8 already
             * says what a connection entering congestion avoidance without a
             * congestion event behind it must do, and that is exactly this
             * case. */
            if (s->tcp) {
                a20_net_cong_apply(s->tcp, (uint8_t)alg);
            }
            return 0;
        }
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        switch (optname) {
        case TCP_NODELAY:
            s->tcp_nodelay = val != 0;
            if (s->tcp) {
                uint64_t flags = a20_lwip_lock();
                if (s->tcp_nodelay)
                    tcp_nagle_disable(s->tcp);
                else
                    tcp_nagle_enable(s->tcp);
                a20_lwip_unlock(flags);
            }
            return 0;
        case TCP_CORK:
        case TCP_MAXSEG:
        case TCP_SYNCNT:
        case TCP_LINGER2:
        case TCP_DEFER_ACCEPT:
        case TCP_WINDOW_CLAMP:
        case TCP_QUICKACK:
        case TCP_USER_TIMEOUT:
            return 0;
        case TCP_KEEPIDLE:
            if (val <= 0)
                return -EINVAL;
            s->keep_idle = val;
            if (s->tcp) {
                uint64_t flags = a20_lwip_lock();
                s->tcp->keep_idle = (u32_t)val * 1000U;
                a20_lwip_unlock(flags);
            }
            return 0;
        case TCP_KEEPINTVL:
            if (val <= 0)
                return -EINVAL;
            s->keep_intvl = val;
            if (s->tcp) {
                uint64_t flags = a20_lwip_lock();
                s->tcp->keep_intvl = (u32_t)val * 1000U;
                a20_lwip_unlock(flags);
            }
            return 0;
        case TCP_KEEPCNT:
            if (val <= 0)
                return -EINVAL;
            s->keep_cnt = val;
            if (s->tcp) {
                uint64_t flags = a20_lwip_lock();
                s->tcp->keep_cnt = (u32_t)val;
                a20_lwip_unlock(flags);
            }
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }
    if (level == SOL_SOCKET && optname == SO_ATTACH_BPF) {
        /* Socket filters are not wired to a KEP extension point yet; the
         * net-packet extension point is the planned home for them. */
        return -EOPNOTSUPP;
    }
    if (level == SOL_SOCKET && optname == SO_RCVTIMEO) {
        if (!optval || optlen < sizeof(long) * 2)
            return -EINVAL;
        s->recv_timeout_ticks = timeval_to_ticks(optval, optlen);
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_SNDTIMEO) {
        if (!optval || optlen < sizeof(long) * 2)
            return -EINVAL;
        s->send_timeout_ticks = timeval_to_ticks(optval, optlen);
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->reuseaddr = val != 0;
        /* setsockopt() normally runs between socket() and bind(), and socket()
         * has already made the pcb, so net_inet_tcp_apply_options() saw the old
         * value.  Push it across here or bind() would consult a stale
         * SOF_REUSEADDR. */
        if (s->tcp) {
            uint64_t flags = a20_lwip_lock();
            if (s->reuseaddr)
                ip_set_option(s->tcp, SOF_REUSEADDR);
            else
                ip_reset_option(s->tcp, SOF_REUSEADDR);
            a20_lwip_unlock(flags);
        }
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_REUSEPORT) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->reuseport = val != 0;
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_KEEPALIVE) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->keepalive = val != 0;
        if (s->tcp) {
            if (s->keepalive)
                s->tcp->so_options |= SOF_KEEPALIVE;
            else
                s->tcp->so_options &= ~SOF_KEEPALIVE;
        }
        return 0;
    }
    if (level == SOL_SOCKET &&
        (optname == SO_SNDBUF || optname == SO_RCVBUF)) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        /* Linux clamps a non-positive request up to a minimal buffer rather
         * than failing; this refuses it instead.  -EINVAL is the honest answer
         * here because a clamped-to-zero buffer cannot send anything, and
         * silently accepting one would leave a caller believing it had asked
         * for a usable size. */
        if (val <= 0)
            return -EINVAL;
        /* Only TCP has a buffer for the option to mean anything about: the
         * ceilings live on the lwIP pcb (snd_buf's available space and the
         * wnd_limit field that gates tcp_recved()).  UDP and RAW keep returning
         * -EOPNOTSUPP below, which is what they already did -- their buffers
         * live in the socket layer and are a different, unsized mechanism. */
        if (s->domain != AF_INET && s->domain != AF_INET6)
            return -EOPNOTSUPP;
        if (s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET)
            return -EOPNOTSUPP;
        uint32_t is_snd = (optname == SO_SNDBUF);
        if (is_snd)
            s->snd_buf = (uint32_t)val;
        else
            s->rcv_buf = (uint32_t)val;
        /* Apply now if a pcb exists, so an established connection does not have
         * to wait for the next connect().  net_inet_tcp_buf_apply() clamps to
         * TCP_SND_BUF / TCP_WND_MAX(pcb) and writes the clamped value back;
         * it is also where "raising a send ceiling takes effect next time" is
         * documented, so do not invent that here. */
        if (s->tcp) {
            uint64_t flags = a20_lwip_lock();
            net_inet_tcp_buf_apply(s, s->tcp);
            a20_lwip_unlock(flags);
        }
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_PASSCRED) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->passcred = val != 0;
        return 0;
    }
    /* Unhandled SOL_SOCKET options (SO_BINDTODEVICE, SO_DOMAIN,
     * SO_PROTOCOL, SO_RXQ_OVFL, ...) are not implemented.  Returning 0 here
     * would report success for a setting that was discarded, which is worse
     * than refusing: a server that sets SO_BINDTODEVICE for tenant isolation
     * would believe it isolated and would not be. */
    return -EOPNOTSUPP;
}

int net_getsockopt(int gfd, int level, int optname, void *optval, size_t *optlen)
{
    return net_getsockopt_sock(net_socket_from_file(gfd), level, optname,
                               optval, optlen);
}

int net_getsockopt_sock(net_socket_t *s, int level, int optname,
                        void *optval, size_t *optlen)
{
    if (!s)
        return -ENOTSOCK;
    if (!optval || !optlen)
        return -EINVAL;
    int val = 0;
    if (level == SOL_SOCKET && optname == SO_TYPE)
        val = s->type;
    else if (level == SOL_SOCKET && optname == SO_ERROR)
        val = 0;
    else if (level == SOL_SOCKET && optname == SO_ACCEPTCONN)
        val = s->listening;
    else if (level == SOL_SOCKET && optname == SO_DOMAIN)
        val = s->domain;
    else if (level == SOL_SOCKET && optname == SO_PROTOCOL)
        val = s->protocol;
    else if (level == SOL_SOCKET &&
             (optname == SO_SNDBUF || optname == SO_RCVBUF))
        /* The value in force, not a constant.  net_inet_tcp_buf_apply() writes
         * the clamped value back into these fields, so this reports a ceiling
         * that was cut down to what the pcb can actually honour rather than
         * echoing back a request the stack silently ignored. */
        val = (int)(optname == SO_SNDBUF ? s->snd_buf : s->rcv_buf);
    else if (level == SOL_SOCKET && optname == SO_REUSEADDR)
        val = s->reuseaddr;
    else if (level == SOL_SOCKET && optname == SO_REUSEPORT)
        val = s->reuseport;
    else if (level == SOL_SOCKET && optname == SO_KEEPALIVE)
        val = s->keepalive;
    else if (level == SOL_SOCKET && optname == SO_PASSCRED)
        val = s->passcred;
    else if (level == SOL_SOCKET) {
        if (optname == SO_PEERCRED) {
            /* Linux struct ucred: { pid, uid, gid }. */
            if (*optlen < 3 * sizeof(int32_t))
                return -EINVAL;
            int32_t cred[3];
            cred[0] = s->peer_pid;
            cred[1] = s->peer_uid;
            cred[2] = s->peer_gid;
            memcpy(optval, cred, sizeof(cred));
            *optlen = sizeof(cred);
            return 0;
        }
        if (optname == SO_PEERPIDFD) {
            /* Linux 6.5+: pidfd pinning the peer; dbus-daemon uses this for
             * peer identity when available. */
            extern int linux_pidfd_create(int pid, int flags);
            if (s->peer_pid <= 0)
                return -ENOTCONN;
            if (*optlen < sizeof(int32_t))
                return -EINVAL;
            int pfd = linux_pidfd_create(s->peer_pid, 0);
            if (pfd < 0)
                return pfd;
            int32_t out = pfd;
            memcpy(optval, &out, sizeof(out));
            *optlen = sizeof(out);
            return 0;
        }
        val = 0;
    }
    else if (level == IPPROTO_IP) {
        /* Report the effective value, not the stored field: a caller that never
         * set an option must see the TTL/TOS its packets actually carry. */
        uint8_t ttl, tos, mc_ttl;
        net_inet_ip_effective(s, &ttl, &tos, &mc_ttl);
        switch (optname) {
        case IP_TTL:                  val = ttl; break;
        case IP_TOS:                  val = tos; break;
        case A20_IP_MULTICAST_TTL_WIRE:  val = mc_ttl; break;
        case A20_IP_MULTICAST_LOOP_WIRE: val = s->mc_loop; break;
        /* -EOPNOTSUPP rather than -ENOPROTOOPT: the option exists in the Linux
         * ABI, this kernel just cannot honour it (IP_OPTIONS needs a
         * per-packet option buffer lwIP's output path has no room for). */
        default: return -EOPNOTSUPP;
        }
    }
    else if (level == IPPROTO_TCP) {
        if (s->type != SOCK_STREAM)
            return -ENOPROTOOPT;
        if (optname == TCP_CONGESTION) {
            /* Report the algorithm this connection will actually use, which is
             * the whole point of the option.  It used to hardcode "reno" (and,
             * before that, "cubic" when no CUBIC existed at all): the first
             * made a caller that had set "cubic" unable to tell whether the
             * request took, the second made monitoring tools believe an
             * implementation existed that did not.
             *
             * A live pcb is authoritative over the stored request, because
             * setsockopt applies to the pcb as well -- a connection that was
             * accepted inherits its listener's algorithm, so the socket's own
             * field is not always the answer. */
            uint8_t alg = s->tcp_congestion;
#if LWIP_TCP_CUBIC
            if (s->tcp) {
                alg = s->tcp->cong_alg;
            }
#else
            /* Without CUBIC there is only ever one algorithm, whatever the
             * socket recorded.  Saying otherwise here would put this
             * getsockopt at odds with the setsockopt above it. */
            alg = TCP_CONG_RENO;
#endif
            const char *name = tcp_cong_alg_name(alg);
            size_t len = strlen(name) + 1;
            size_t n = *optlen < len ? *optlen : len;
            if (n)
                memcpy(optval, name, n);
            *optlen = n;
            return 0;
        }
        if (optname == TCP_INFO) {
            /* No struct tcp_info is modelled.  The old handler wrote a state
             * byte at offset 0 and zeroed the rest, which callers read as
             * tcpi_rtt/tcpi_total_retrans being measured-and-zero.  Refuse. */
            return -EOPNOTSUPP;
        }
        switch (optname) {
        case TCP_NODELAY:
            val = s->tcp_nodelay;
            break;
        case TCP_MAXSEG:
            val = 1460;
            break;
        case TCP_CORK:
        case TCP_SYNCNT:
        case TCP_LINGER2:
        case TCP_DEFER_ACCEPT:
        case TCP_WINDOW_CLAMP:
        case TCP_QUICKACK:
        case TCP_USER_TIMEOUT:
            val = 0;
            break;
        case TCP_KEEPIDLE:
            val = s->keep_idle > 0 ? s->keep_idle :
                  (s->tcp ? (int)(s->tcp->keep_idle / 1000U) : 7200);
            break;
        case TCP_KEEPINTVL:
            val = s->keep_intvl > 0 ? s->keep_intvl :
                  (s->tcp ? (int)(s->tcp->keep_intvl / 1000U) : 75);
            break;
        case TCP_KEEPCNT:
            val = s->keep_cnt > 0 ? s->keep_cnt :
                  (s->tcp ? (int)s->tcp->keep_cnt : 9);
            break;
        default:
            return -ENOPROTOOPT;
        }
    }
    else if (s->domain == AF_INET6 && level == IPPROTO_IPV6) {
        switch (optname) {
        case IPV6_V6ONLY:          val = 0; break;
        case IPV6_RECVPKTINFO:     val = s->ipv6_recv_pktinfo; break;
        case IPV6_RECVTCLASS:      val = s->ipv6_recv_tclass; break;
        case IPV6_RECVHOPLIMIT:    val = s->ipv6_recv_hoplimit; break;
        case IPV6_RECVRTHDR:       val = s->ipv6_recv_rthdr; break;
        case IPV6_RECVHOPOPTS:     val = s->ipv6_recv_hopopts; break;
        case IPV6_RECVDSTOPTS:     val = s->ipv6_recv_dstopts; break;
        case IPV6_RECVERR:         val = s->ipv6_recv_err; break;
        case IPV6_2292PKTINFO:     val = s->ipv6_recv_2292_pktinfo; break;
        case IPV6_2292HOPLIMIT:    val = s->ipv6_recv_2292_hoplimit; break;
        case IPV6_2292RTHDR:       val = s->ipv6_recv_2292_rthdr; break;
        case IPV6_2292HOPOPTS:     val = s->ipv6_recv_2292_hopopts; break;
        case IPV6_2292DSTOPTS:     val = s->ipv6_recv_2292_dstopts; break;
        case IPV6_UNICAST_IF:      val = 0; break;
        case IPV6_MULTICAST_IF:    val = 0; break;
        case IPV6_MULTICAST_HOPS:  val = -1; break;
        case IPV6_MULTICAST_LOOP:  val = 1; break;
        case IPV6_TCLASS:          val = 0; break;
        case IPV6_HOPLIMIT:        val = -1; break;
        case IPV6_FLOWINFO:        val = 0; break;
        default: return -ENOPROTOOPT;
        }
    }
    else
        return -EOPNOTSUPP;
    return net_copyout_int(optval, optlen, val);
}

int net_shutdown(int gfd, int how)
{
    return net_shutdown_sock(net_socket_from_file(gfd), how);
}

int net_shutdown_sock(net_socket_t *s, int how)
{
    if (!s)
        return -ENOTSOCK;
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    net_socket_t *peer = NULL;
    bool drain_accept = false;
    bool drain_read = false;
    bool drain_write = false;
    bool drain_peer_read = false;
    bool drain_peer_write = false;
    /* s and its peer: two buckets, ascending.  s->peer is a plain back-pointer
     * with no reference of its own, so it is sampled and referenced under s's
     * own bucket; reading it before any lock could name a socket another CPU
     * has already freed.  A peer unregistered in the window between the two
     * steps lands in the orphan shard, which is the one the pair then holds,
     * and the validity check below rejects it. */
    int sb = net_socket_bucket(s);
    {
        uint64_t sf = net_bucket_lock(sb);
        if (s->peer)
            peer = net_socket_ref(s->peer);
        net_bucket_unlock(sb, sf);
    }
    net_bucket_pair_t pair = net_bucket_lock2(sb, net_socket_bucket(peer));

    if (how == SHUT_RDWR) {
        s->closed = 1;
        s->shut_rd = 1;
        s->shut_wr = 1;
    }
    if (how == SHUT_WR) {
        s->shut_wr = 1;
    }
    if (how == SHUT_RD) {
        s->shut_rd = 1;
        net_msg_t *m = s->rx_head;
        s->rx_head = s->rx_tail = NULL;
        s->rx_count = 0;
        net_bucket_unlock2(pair);
        while (m) {
            net_msg_t *next = m->next;
            net_msg_free(m);
            m = next;
        }
        pair = net_bucket_lock2(sb, net_socket_bucket(peer));
    }

    drain_accept = net_wait_queue_collect_all_locked(
        &s->accept_waitq, PROC_WAKE_EVENT, &wake_q);
    drain_read = net_wait_queue_collect_all_locked(
        &s->read_waitq, PROC_WAKE_EVENT, &wake_q);
    drain_write = net_wait_queue_collect_all_locked(
        &s->write_waitq, PROC_WAKE_EVENT, &wake_q);

    if (peer && (s->type == SOCK_STREAM || s->type == SOCK_SEQPACKET ||
                 net_socket_is_live(peer)) && peer->peer == s) {
        if (how == SHUT_WR || how == SHUT_RDWR) {
            peer->peer_closed = 1;
            drain_peer_read = net_wait_queue_collect_all_locked(
                &peer->read_waitq, PROC_WAKE_EVENT, &wake_q);
        }
        drain_peer_write = net_wait_queue_collect_all_locked(
            &peer->write_waitq, PROC_WAKE_EVENT, &wake_q);
    }
    net_bucket_unlock2(pair);
    (void)proc_wake_q_flush(&wake_q);
    if (drain_accept)
        (void)wait_queue_wake_all(
            &s->accept_waitq, 0, PROC_WAKE_EVENT);
    if (drain_read)
        (void)wait_queue_wake_all(
            &s->read_waitq, 0, PROC_WAKE_EVENT);
    if (drain_write)
        (void)wait_queue_wake_all(
            &s->write_waitq, 0, PROC_WAKE_EVENT);
    if (drain_peer_read)
        (void)wait_queue_wake_all(
            &peer->read_waitq, 0, PROC_WAKE_EVENT);
    if (drain_peer_write)
        (void)wait_queue_wake_all(
            &peer->write_waitq, 0, PROC_WAKE_EVENT);
    if (peer)
        net_socket_free(peer);
    return 0;
}

int net_set_nonblock_vfile(vfile_t *vf, int nonblock)
{
    net_socket_t *s = vf && net_is_socket_vfile(vf) ? vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    int b = net_socket_bucket(s);
    uint64_t irq = net_bucket_lock(b);
    s->nonblock = nonblock ? 1 : 0;
    net_bucket_unlock(b, irq);
    return 0;
}

int net_set_nonblock(int gfd, int nonblock)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    int b = net_socket_bucket(s);
    uint64_t irq = net_bucket_lock(b);
    s->nonblock = nonblock ? 1 : 0;
    net_bucket_unlock(b, irq);
    return 0;
}

int net_poll_file(vfile_t *vf, short events)
{
    net_socket_t *s = vf && net_is_socket_vfile(vf) ? vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    short revents = 0;
    int b = net_socket_bucket(s);
    uint64_t irq = net_bucket_lock(b);
    if (s->ch_ep) {
        int ch_rd = a20_channel_readable(s->ch_ep) || s->ch_len > 0;
        int ch_wr = a20_channel_writable(s->ch_ep);
        int ch_pc = a20_channel_peer_closed(s->ch_ep);
        if (s->peer_closed || ch_pc)
            revents |= POLLHUP;
        if ((events & POLLIN) &&
            (s->rx_head || ch_rd || s->closed || s->peer_closed || ch_pc ||
             s->shut_rd))
            revents |= POLLIN;
        if ((events & POLLOUT) && !s->closed && !s->shut_wr) {
            if (s->peer_closed || ch_pc)
                revents |= POLLERR;
            else if ((s->domain == AF_UNIX || s->local_tcp) &&
                     s->type == SOCK_STREAM && s->connected && !s->peer)
                revents |= POLLERR;
            else if (ch_wr || s->rx_count < NET_MAX_QUEUE)
                revents |= POLLOUT;
        }
        net_bucket_unlock(b, irq);
        return revents;
    }
    if (s->peer_closed)
        revents |= POLLHUP;
    else if (s->closed || (s->shut_rd && s->shut_wr))
        revents |= POLLHUP;
    if ((events & POLLIN) &&
        (s->rx_head || s->accept_head || s->closed || s->peer_closed || s->shut_rd))
        revents |= POLLIN;
    if ((events & POLLOUT) && !s->closed && !s->shut_wr) {
        if (s->peer_closed)
            revents |= POLLERR;
        else if (s->tcp_connecting)
            revents |= 0;
        else if ((s->domain == AF_UNIX || s->local_tcp) &&
                 s->type == SOCK_STREAM && s->connected && !s->peer)
            revents |= POLLERR;
        else if (s->peer && s->type == SOCK_STREAM && s->peer->rx_count >= NET_MAX_QUEUE)
            /* Deliberately a weakly consistent read: the peer may well be in
             * another bucket, and taking it here would turn poll(2) into a
             * reader that contends with the peer's data path.  The only
             * consequence of reading a slightly stale rx_count is one extra or
             * one missing POLLOUT in this single call. */
            revents |= 0;
        else
            revents |= POLLOUT;
    }
    net_bucket_unlock(b, irq);
    return revents;
}

int net_poll_events(int gfd, short events)
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -ENOTSOCK;
    int revents = net_poll_file(vf, events);
    vfs_put_file_ref(gfd, vf);
    return revents;
}
