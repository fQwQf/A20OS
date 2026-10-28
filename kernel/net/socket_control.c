#include "net/socket_internal.h"
#include "ipc/ipc.h"
#include "core/klog.h"
#include "core/string.h"
#include "net/lwip_stack.h"
#include "net/net_config.h"
#include "net/socket_options_state.h"
#include "lwip/tcp.h"
#include "lwip/igmp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip.h"
#include <limits.h>
#ifndef EDOM
#define EDOM 33
#endif
#ifdef CONFIG_LINUX_MMSG_TXN_RUST
#include "abi/linux/mmsg_transaction.h"
extern int a20_linux_timeval_to_ticks(int64_t sec, int64_t usec,
                                      uint64_t tps, uint64_t *out_ticks);
extern int a20_linux_ticks_to_timeval(uint64_t ticks, uint64_t tps,
                                      int64_t *sec_out, int64_t *usec_out);
#endif
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

static void net_tcp_options_apply_current(net_socket_t *s, unsigned mask)
{
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
        if (s->tcp) {
            if (mask & NET_TCP_APPLY_KEEPALIVE) {
                if (opts.keepalive)
                    s->tcp->so_options |= SOF_KEEPALIVE;
                else
                    s->tcp->so_options &= ~SOF_KEEPALIVE;
            }
            if (mask & NET_TCP_APPLY_REUSEADDR) {
                if (opts.reuseaddr)
                    ip_set_option(s->tcp, SOF_REUSEADDR);
                else
                    ip_reset_option(s->tcp, SOF_REUSEADDR);
            }
            /* LISTEN uses tcp_pcb_listen, which only has the common PCB
             * prefix.  Its child options live in net_socket_t until accept
             * creates a full tcp_pcb. */
            if (s->tcp->state != LISTEN) {
                if (mask & NET_TCP_APPLY_CONGESTION)
                    a20_net_cong_apply(s->tcp, opts.congestion);
                if (mask & NET_TCP_APPLY_NODELAY) {
                    if (opts.nodelay)
                        tcp_nagle_disable(s->tcp);
                    else
                        tcp_nagle_enable(s->tcp);
                }
                if ((mask & NET_TCP_APPLY_KEEP_IDLE) && opts.keep_idle > 0)
                    s->tcp->keep_idle = (u32_t)opts.keep_idle * 1000U;
                if ((mask & NET_TCP_APPLY_KEEP_INTVL) && opts.keep_intvl > 0)
                    s->tcp->keep_intvl = (u32_t)opts.keep_intvl * 1000U;
                if ((mask & NET_TCP_APPLY_KEEP_CNT) && opts.keep_cnt > 0)
                    s->tcp->keep_cnt = (u32_t)opts.keep_cnt;
                net_inet_tcp_buf_apply(
                    (mask & NET_TCP_APPLY_SNDBUF) ? opts.snd_buf : 0,
                    (mask & NET_TCP_APPLY_RCVBUF) ? opts.rcv_buf : 0,
                    s->tcp);
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
        resource_ns_is_initial(s->net_ns) && g_a20_tcp_path == A20_TCP_PATH_LWIP) {
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


/*
 * Primitives for the Rust accept transaction (kernel/net/socket_accept.rs).
 * Each one is a step the Rust decision surface cannot perform itself: the
 * algorithm accept, the queue pop, a lock, the park protocol, a child field
 * read or write, the install, the registry release, a reference or a free, a
 * wait-queue drain or wake, and the inet destroy.
 */
#include "net/socket_accept_state.h"

_Static_assert(sizeof(net_sock_pair_t) <= sizeof(net_accept_pair_t),
               "net_accept_pair_t must mirror net_sock_pair_t");
_Static_assert(sizeof(proc_wake_q_t) <= 2048,
               "the Rust accept frame must hold a proc_wake_q_t");

void a20_net_accept_txn_view_init(void *sp, net_accept_view_t *out) {
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->listening = &s->listening;
    out->nonblock = &s->nonblock;
    out->closed = &s->closed;
    out->recv_timeout_ticks = &s->recv_timeout_ticks;
    out->accept_head = (void **)&s->accept_head;
}

int a20_net_accept_txn_alg(void *sp, size_t *addrlen, int flags) {
    return net_alg_socket_accept((net_socket_t *)sp, addrlen, flags);
}

uint64_t a20_net_accept_txn_lock(void *sp) { return net_sock_lock((net_socket_t *)sp); }

void a20_net_accept_txn_unlock(void *sp, uint64_t flags) {
    net_sock_unlock((net_socket_t *)sp, flags);
}

void *a20_net_accept_txn_pop(void *sp) {
    return net_accept_queue_pop_locked((net_socket_t *)sp);
}

uint64_t a20_net_accept_txn_now(void) { return timer_get_ticks(); }

int a20_net_accept_txn_have_task(void) { return proc_current() != NULL; }

int a20_net_accept_txn_has_signal(void) {
    task_t *cur = proc_current();
    return cur ? net_task_has_unblocked_signal(cur) : 0;
}

int a20_net_accept_txn_wait_expired_locked(void *sp, uint64_t start) {
    return net_socket_wait_expired_locked((net_socket_t *)sp, start, 0);
}

/*
 * The park for one retry of the accept loop.  The re-check is inside it
 * because it has to happen between the prepare and the link: an accepted child
 * arriving in that window would otherwise wake nobody and the accept would
 * sleep forever.  Returns 0 to retry, -EAGAIN when the wait timed out or no
 * task could park, or -ERESTARTSYS when the park was interrupted (the legacy
 * contract also reports a signal seen under the lock as -ERESTARTSYS).
 */
int a20_net_accept_txn_park(void *sp, uint64_t deadline) {
    net_socket_t *s = (net_socket_t *)sp;
    proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
    if (!token.task)
        return -EAGAIN;
    wait_queue_entry_t entry = {0};
    uint64_t irq = net_sock_lock(s);
    if (s->closed || s->accept_head) {
        net_sock_unlock(s, irq);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return 0;
    }
    if (net_task_has_unblocked_signal(proc_current())) {
        net_sock_unlock(s, irq);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return -ERESTARTSYS;
    }
    bool linked = wait_queue_link(&s->accept_waitq, &entry, token, 0);
    net_sock_unlock(s, irq);
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
        net_task_has_unblocked_signal(proc_current()))
        return -ERESTARTSYS;
    if (reason == PROC_WAKE_TIMEOUT)
        return -EAGAIN;
    return 0;
}

void a20_net_accept_txn_child_ready(void *child) {
    net_inet_accept_child_ready((net_socket_t *)child);
}

size_t a20_net_accept_txn_child_peer_len(void *child) {
    return ((net_socket_t *)child)->peer_len;
}

void a20_net_accept_txn_child_peer_copy(void *child, void *addr, size_t n) {
    memcpy(addr, ((net_socket_t *)child)->peer_addr, n);
}

void a20_net_accept_txn_child_set_nonblock(void *child, int nonblock) {
    ((net_socket_t *)child)->nonblock = nonblock ? 1 : 0;
}

int a20_net_accept_txn_install(void *child) {
    net_socket_t *c = (net_socket_t *)child;
    return net_socket_install_file(c, O_RDWR | (c->nonblock ? O_NONBLOCK : 0));
}

/*
 * child->peer is a plain back-pointer with no reference of its own, so it is
 * sampled and referenced under the child's own lock before the pair is taken.
 */
void a20_net_accept_txn_snapshot_peer(void *child, void **out_peer) {
    net_socket_t *c = (net_socket_t *)child;
    net_socket_t *peer = NULL;
    uint64_t cf = net_sock_lock(c);
    if (c->peer)
        peer = net_socket_ref(c->peer);
    net_sock_unlock(c, cf);
    *out_peer = peer;
}

void a20_net_accept_txn_lock2(void *a, void *b, net_accept_pair_t *out) {
    net_sock_pair_t p = net_sock_lock2((net_socket_t *)a, (net_socket_t *)b);
    out->lo = p.lo;
    out->hi = p.hi;
    out->flags = p.flags;
}

void a20_net_accept_txn_unlock2(const net_accept_pair_t *in) {
    net_sock_pair_t p;
    p.lo = in->lo;
    p.hi = in->hi;
    p.flags = in->flags;
    net_sock_unlock2(p);
}

void a20_net_accept_txn_child_mark_closed(void *child) {
    ((net_socket_t *)child)->closed = 1;
}

/* True when the peer's back-pointer named this child, so its readers and
 * writers now see a peer that is gone and have to be drained and woken. */
int a20_net_accept_txn_peer_detach(void *peer, void *child) {
    net_socket_t *p = (net_socket_t *)peer;
    if (p->peer != (net_socket_t *)child)
        return 0;
    p->peer = NULL;
    p->peer_closed = 1;
    return 1;
}

int a20_net_accept_txn_peer_collect_read(void *peer, void *wake_q) {
    return net_wait_queue_collect_all_locked(&((net_socket_t *)peer)->read_waitq,
                                             PROC_WAKE_EVENT,
                                             (proc_wake_q_t *)wake_q);
}

int a20_net_accept_txn_peer_collect_write(void *peer, void *wake_q) {
    return net_wait_queue_collect_all_locked(&((net_socket_t *)peer)->write_waitq,
                                             PROC_WAKE_EVENT,
                                             (proc_wake_q_t *)wake_q);
}

int a20_net_accept_txn_unregister(void *child) {
    return net_socket_unregister((net_socket_t *)child) ? 1 : 0;
}

void a20_net_accept_txn_free(void *sp) { net_socket_free((net_socket_t *)sp); }

void a20_net_accept_txn_child_destroy(void *child) {
    net_inet_socket_destroy((net_socket_t *)child);
}

void a20_net_accept_txn_peer_wake_read(void *peer) {
    (void)wait_queue_wake_all(&((net_socket_t *)peer)->read_waitq, 0,
                              PROC_WAKE_EVENT);
}

void a20_net_accept_txn_peer_wake_write(void *peer) {
    (void)wait_queue_wake_all(&((net_socket_t *)peer)->write_waitq, 0,
                              PROC_WAKE_EVENT);
}

void a20_net_accept_txn_wakeq_init(void *wake_q) {
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void a20_net_accept_txn_wakeq_flush(void *wake_q) {
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

int net_accept_sock(net_socket_t *s, void *addr, size_t *addrlen, int flags)
{
    return a20_net_accept_sock(s, addr, addrlen, flags);
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
    uint64_t irq = net_sock_lock(s);
    if (!net_socket_is_live(s)) {
        net_sock_unlock(s, irq);
        return -ENOTSOCK;
    }
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    size_t n = s->local_len < *addrlen ? s->local_len : *addrlen;
    memcpy(addr, s->local, n);
    *addrlen = n;
    net_sock_unlock(s, irq);
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
        return net_notconn(NET_NOTCONN_GETPEERNAME);
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


int net_getsockopt(int gfd, int level, int optname, void *optval, size_t *optlen)
{
    return net_getsockopt_sock(net_socket_from_file(gfd), level, optname,
                               optval, optlen);
}



/*
 * ------------------------------------------------------------------
 * Rust option state machine (NET_SOCKET_OPTIONS_IMPL=rust)
 * ------------------------------------------------------------------
 *
 * What stays here is the primitive half: the borrowed view into net_socket_t,
 * the socket lock and lwIP barrier the Rust module decides *when* to take, the
 * buffer-ceiling and IGMP lookups (lwIP state), the live-pcb reads and the raw
 * copyout. What moved to kernel/net/socket_options.rs is every decision: which
 * option is recognised at which level, its accepted range, the byte-order
 * re-read, the clamp, which field it lands in, which pcb-apply group it
 * invalidates, and what getsockopt reports.
 *
 * The view is a table of pointers into the caller's socket, so no struct ABI
 * moves and the C reference above stays byte-comparable with the Rust state
 * machine (see tools/tests/socket_options_host).
 */

/* The Rust side indexes the twelve IPv6 RECV flags as one run of int. */
_Static_assert(__builtin_offsetof(net_socket_t, ipv6_recv_2292_dstopts) -
                   __builtin_offsetof(net_socket_t, ipv6_recv_pktinfo) ==
               11 * sizeof(int),
               "the IPv6 RECV* flags must stay one contiguous run of int: the "
               "Rust option state machine indexes them as such");

static void net_option_view_init(net_socket_t *s, net_option_view_t *v)
{
    memset(v, 0, sizeof(*v));
    v->s = s;
    v->domain = &s->domain;
    v->sock_type = &s->type;
    v->protocol = &s->protocol;
    v->connected = &s->connected;
    v->bound = &s->bound;
    v->listening = &s->listening;
    v->ip_ttl = &s->ip_ttl;
    v->ip_tos = &s->ip_tos;
    v->ip_ttl_set = &s->ip_ttl_set;
    v->ip_tos_set = &s->ip_tos_set;
    v->mc_ttl = &s->mc_ttl;
    v->mc_loop = &s->mc_loop;
    v->ip_opts_generation = &s->ip_opts_generation;
    v->ipv6_checksum_offset = &s->ipv6_checksum_offset;
    v->ipv6_v6only = &s->ipv6_v6only;
    v->ipv6_recv_flags = &s->ipv6_recv_pktinfo;
    v->icmp6_filter = s->icmp6_filter;
    v->icmp6_filter_set = &s->icmp6_filter_set;
    v->tcp_congestion = &s->tcp_congestion;
    v->tcp_nodelay = &s->tcp_nodelay;
    v->keepalive = &s->keepalive;
    v->keep_idle = &s->keep_idle;
    v->keep_intvl = &s->keep_intvl;
    v->keep_cnt = &s->keep_cnt;
    v->tcp_opts_generation = &s->tcp_opts_generation;
    v->snd_buf = &s->snd_buf;
    v->rcv_buf = &s->rcv_buf;
    v->reuseaddr = &s->reuseaddr;
    v->reuseport = &s->reuseport;
    v->recv_timeout_ticks = &s->recv_timeout_ticks;
    v->send_timeout_ticks = &s->send_timeout_ticks;
    v->passcred = &s->passcred;
    v->peer_pid = &s->peer_pid;
    v->peer_uid = &s->peer_uid;
    v->peer_gid = &s->peer_gid;
    v->long_size = (int)sizeof(long);
    /* A macro that expands to a function call on riscv64: the Rust side cannot
     * know it, so it is handed over with the rest of the view. */
    v->ticks_per_sec = (uint64_t)TICKS_PER_SEC;
}

void a20_net_option_lock(void *sp, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)sp);
}

void a20_net_option_unlock(void *sp, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)sp, flags);
}

uint32_t a20_net_option_buf_ceiling(void *sp, int is_snd)
{
    return net_socket_buf_ceiling((net_socket_t *)sp, is_snd);
}

int a20_net_option_group_membership(void *sp, const void *mreq, int join)
{
    return net_ip_group_membership((net_socket_t *)sp,
                                   (const net_ip_mreqn_t *)mreq, join);
}

int a20_net_option_notconn_pidfd(void)
{
    return net_notconn(NET_NOTCONN_PEERPIDFD);
}

/* Live pcb values, read under the lwIP barrier and never under a socket lock.
 * `which`: 0 congestion algorithm, 1 keep_idle (s), 2 keep_intvl (s),
 * 3 keep_cnt.  -1 means "no usable pcb", which is a NULL pcb or LISTEN (whose
 * pcb only carries the common prefix).  With CUBIC compiled out there is one
 * algorithm and the congestion case reports it, which is what the old handler
 * did. */
int a20_net_option_tcp_live(void *sp, int which)
{
    net_socket_t *s = (net_socket_t *)sp;
    if (which == 0) {
#if LWIP_TCP_CUBIC
        uint64_t flags = a20_lwip_lock();
        int alg = (s->tcp && s->tcp->state != LISTEN)
                      ? (int)s->tcp->cong_alg : -1;
        a20_lwip_unlock(flags);
        return alg;
#else
        return TCP_CONG_RENO;
#endif
    }
    uint64_t flags = a20_lwip_lock();
    int v = -1;
    if (s->tcp && s->tcp->state != LISTEN) {
        switch (which) {
        case 1: v = (int)(s->tcp->keep_idle / 1000U); break;
        case 2: v = (int)(s->tcp->keep_intvl / 1000U); break;
        case 3: v = (int)s->tcp->keep_cnt; break;
        default: break;
        }
    }
    a20_lwip_unlock(flags);
    return v;
}

/* The TTL/TOS/multicast-TTL a packet would actually carry.  For an option the
 * caller never set that is the pcb's value, not the stored zero. */
int a20_net_option_ip_effective(void *sp, int which)
{
    uint8_t ttl, tos, mc_ttl;
    net_inet_ip_effective((net_socket_t *)sp, &ttl, &tos, &mc_ttl);
    switch (which) {
    case 0: return ttl;
    case 1: return tos;
    default: return mc_ttl;
    }
}

extern int linux_pidfd_create(int pid, int flags);

int net_setsockopt_sock(net_socket_t *s, int level, int optname,
                        const void *optval, size_t optlen)
{
    if (!s)
        return -ENOTSOCK;
    net_option_view_t v;
    net_option_view_init(s, &v);
    int r = a20_net_setsockopt_state(&v, level, optname, optval, optlen);
    if (r < 0)
        return r;
    /* An explicit request is final for that direction; the growth policy is
     * only for callers that never expressed a preference. */
    if (level == SOL_SOCKET && optname == SO_SNDBUF)
        s->snd_autotune = 0;
    else if (level == SOL_SOCKET && optname == SO_RCVBUF)
        s->rcv_autotune = 0;
    if (r & NET_OPT_APPLY_IP_OPTS)
        net_inet_ip_opts_apply(s);
    if (r & NET_OPT_APPLY_PCB_MASK)
        net_tcp_options_apply_current(s, (unsigned)(r & NET_OPT_APPLY_PCB_MASK));
    return 0;
}

int net_getsockopt_sock(net_socket_t *s, int level, int optname,
                        void *optval, size_t *optlen)
{
    if (!s)
        return -ENOTSOCK;
    if (!optval || !optlen)
        return -EINVAL;
    net_option_view_t v;
    net_option_view_init(s, &v);
    net_opt_report_t rep;
    memset(&rep, 0, sizeof(rep));
    int r = a20_net_getsockopt_state(&v, level, optname, *optlen, &rep);
    if (r < 0)
        return r;
    if (level == SOL_SOCKET && optname == SO_ERROR) {
        /* Validate the output shape before consuming this shared socket state.
         * Aliases must observe the error once, even when queried concurrently. */
        uint64_t irq = net_sock_lock(s);
        rep.val0 = s->pending_error;
        s->pending_error = 0;
        net_sock_unlock(s, irq);
    }
    switch (rep.kind) {
    case NET_OPT_REPORT_INT: {
        int val = (int)rep.val0;
        memcpy(optval, &val, sizeof(val));
        *optlen = sizeof(val);
        return 0;
    }
    case NET_OPT_REPORT_STRING:
        if (rep.len)
            memcpy(optval, rep.text, rep.len);
        *optlen = rep.len;
        return 0;
    case NET_OPT_REPORT_CRED: {
        int32_t cred[3] = { (int32_t)rep.val0, (int32_t)rep.val1,
                            (int32_t)rep.val2 };
        memcpy(optval, cred, sizeof(cred));
        *optlen = sizeof(cred);
        return 0;
    }
    case NET_OPT_REPORT_TIMEVAL64: {
        int64_t tv[2] = { rep.val0, rep.val1 };
        if (rep.len) memcpy(optval, tv, rep.len);
        *optlen = rep.len;
        return 0;
    }
    case NET_OPT_REPORT_TIMEVAL: {
        long tv[2] = { (long)rep.val0, (long)rep.val1 };
        if (rep.len)
            memcpy(optval, tv, rep.len);
        *optlen = rep.len;
        return 0;
    }
    case NET_OPT_REPORT_PIDFD: {
        int pfd = linux_pidfd_create((int)rep.val0, O_CLOEXEC);
        if (pfd < 0)
            return pfd;
        int32_t out = pfd;
        memcpy(optval, &out, sizeof(out));
        *optlen = sizeof(out);
        return 0;
    }
    default:
        return -EINVAL;
    }
}


int net_shutdown(int gfd, int how)
{
    return net_shutdown_sock(net_socket_from_file(gfd), how);
}

int net_shutdown_sock(net_socket_t *s, int how)
{
    if (!s)
        return -ENOTSOCK;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    int transport = net_inet_shutdown(s, how != SHUT_WR, how != SHUT_RD);
    if (transport < 0) return transport;
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    net_socket_t *peer = NULL;
    bool drain_accept = false;
    bool drain_read = false;
    bool drain_write = false;
    bool drain_peer_read = false;
    bool drain_peer_write = false;
    /* s and its peer: two socket locks, ascending by address.  s->peer is a plain
     * back-pointer with no reference of its own, so it is sampled and
     * referenced under s's own lock; reading it before any lock could name a
     * socket another CPU has already freed.  That reference is what pins the
     * peer for the pair below, whether or not it still holds a registry slot
     * -- the old code leaned on the orphan shard for exactly that. */
    {
        uint64_t sf = net_sock_lock(s);
        if (s->peer)
            peer = net_socket_ref(s->peer);
        net_sock_unlock(s, sf);
    }
    net_sock_pair_t pair = net_sock_lock2(s, peer);

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
        net_msg_t *m = net_rxq_detach_locked(s);
        net_sock_unlock2(pair);
        while (m) {
            net_msg_t *next = m->next;
            net_msg_free(m);
            m = next;
        }
        pair = net_sock_lock2(s, peer);
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
    net_sock_unlock2(pair);
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
    uint64_t irq = net_sock_lock(s);
    s->nonblock = nonblock ? 1 : 0;
    net_sock_unlock(s, irq);
    return 0;
}

int net_set_nonblock(int gfd, int nonblock)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    uint64_t irq = net_sock_lock(s);
    s->nonblock = nonblock ? 1 : 0;
    net_sock_unlock(s, irq);
    return 0;
}


/*
 * Primitives for the Rust poll readiness computation
 * (kernel/net/socket_poll.rs).  Each one is a step the Rust decision surface
 * cannot perform itself: the vfile-to-socket resolution, a lock, the channel
 * tests, the peer queue depth read and the queue capacity.
 */
#include "net/socket_poll_state.h"

void *a20_net_poll_socket_from_vfile(void *vf) {
    vfile_t *f = (vfile_t *)vf;
    return (f && net_is_socket_vfile(f)) ? f->priv : NULL;
}

void a20_net_poll_view_init(void *sp, net_poll_view_t *out) {
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->closed = &s->closed;
    out->shut_rd = &s->shut_rd;
    out->shut_wr = &s->shut_wr;
    out->peer_closed = &s->peer_closed;
    out->connected = &s->connected;
    out->local_tcp = &s->local_tcp;
    out->tcp_connecting = &s->tcp_connecting;
    out->rx_count = &s->rx_count;
    out->ch_ep = (void **)&s->ch_ep;
    out->ch_len = &s->ch_len;
    out->rx_head = (void **)&s->rx_head;
    out->accept_head = (void **)&s->accept_head;
    out->peer = (void **)&s->peer;
}

uint64_t a20_net_poll_lock(void *sp) { return net_sock_lock((net_socket_t *)sp); }

void a20_net_poll_unlock(void *sp, uint64_t flags) {
    net_sock_unlock((net_socket_t *)sp, flags);
}

int a20_net_poll_channel_readable(void *ch_ep) {
    return a20_channel_readable((a20_channel_ep_t *)ch_ep) ? 1 : 0;
}

int a20_net_poll_channel_writable(void *ch_ep) {
    return a20_channel_writable((a20_channel_ep_t *)ch_ep) ? 1 : 0;
}

int a20_net_poll_channel_peer_closed(void *ch_ep) {
    return a20_channel_peer_closed((a20_channel_ep_t *)ch_ep) ? 1 : 0;
}

/* A weakly consistent read of the peer's queue depth; see the Rust comment for
 * why poll(2) must not take the peer's lock for it. */
int a20_net_poll_peer_rx_count(void *peer) {
    return ((net_socket_t *)peer)->rx_count;
}

int a20_net_poll_max_queue(void) { return NET_MAX_QUEUE; }

int net_poll_file(vfile_t *vf, short events)
{
    return a20_net_poll_file(vf, events);
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
