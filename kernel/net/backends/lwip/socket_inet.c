#include "tun_private.h"
#include "native.h"
#include "net/tun.h"
#include "net/lwip_stack.h"
#include "net/lwip_concurrency.h"
#include "net/net_config.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "core/klog.h"
#include "core/perf.h"
#include "core/string.h"
#include "core/timer.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#if LWIP_TCP_CUBIC
/* TCP_CONG_* values for the per-socket congestion control selection. */
#include "lwip/priv/tcp_cubic_priv.h"
#endif
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/prot/icmp.h"
#include "native.h"

/*
 * Take the lane currently recorded on a socket, then validate it against the
 * PCB while that lane excludes every cold writer.  Bind/listen/connect/close
 * take the all-lane control barrier, so the PCB pointer and its lane are stable
 * for this check.  A stale socket snapshot is retried on the PCB's owning lane.
 * Raw PCBs are deliberately not bucketed; their socket lane is authoritative.
 */
uint64_t net_inet_socket_lane_lock(net_socket_t *s)
{
    unsigned lane = net_socket_lane_load(s);
    for (;;) {
        uint64_t flags = a20_lwip_lane_lock(lane);
        /* A cold publisher may have changed lw(s)->lane while this caller was
         * waiting for the snapshot lane.  Do not dereference a PCB until the
         * acquired lane still matches the published socket owner: otherwise a
         * completed rebucket/close could have moved or freed it on another
         * lane. */
        unsigned published = net_socket_lane_load(s);
        if (published != lane) {
            a20_lwip_lane_unlock(flags);
            lane = published;
            continue;
        }
        unsigned owner = lane;
        if (lw(s)->tcp)
            owner = NET_PCB_LANE_OWNER_OF_PCB(lw(s)->tcp);
        else if (lw(s)->udp)
            owner = NET_PCB_LANE_OWNER_OF_PCB(lw(s)->udp);
        else if (net_socket_lane_load(s) != lane)
            owner = net_socket_lane_load(s);
        if (owner == lane)
            return flags;
        a20_lwip_lane_unlock(flags);
        lane = owner;
    }
}


static uint16_t net_htons(uint16_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(value);
#else
    return value;
#endif
}

int a20_lwip_owns_ipv4(uint32_t addr)
{
    /* INADDR_ANY: bind() means "any local address", always available.
     * 127.0.0.1: loopback is up before any interface is configured. */
    if (addr == 0 || addr == 0x0100007fU)
        return 1;

    /* Anything else has to be an address this host actually holds, so the
     * question is asked of the netif list rather than answered from a table of
     * literals.  The previous version accepted only 0x0f02000aU, which is
     * 10.0.2.15 with its bytes reversed and so is not the network-byte-order
     * value a sockaddr_in carries: binding the address the guest was actually
     * configured with returned EADDRNOTAVAIL, and any bind that did match the
     * literal was a bind to an address no interface held.  sin_addr is compared
     * as it arrives (no ntohl), because user space hands bind() the address in
     * network byte order and netif_ip4_addr(n)->addr is stored the same way --
     * the same comparison net_ip_group_netif() makes (socket_control.c:120).
     *
     * g_lwip_lock alone: netif_list is lwIP state, and the two network locks
     * must never be held together.  net_bind_sock() calls this before taking
     * the socket's own lock, and nothing here allocates or sleeps. */
    uint64_t flags = a20_lwip_lock();
    int local = 0;
    for (struct netif *n = netif_list; n; n = n->next) {
        /* A TUN netif keeps state == NULL on purpose, but its address is as
         * local as any other: bind(), connect()'s local-target test and the
         * fast loopback path all ask this question. */
        if ((n->state || a20_lwip_netif_is_tun(n)) &&
            netif_ip4_addr(n)->addr == addr) {
            local = 1;
            break;
        }
    }
    a20_lwip_unlock(flags);
    return local;
}

static int net_inet_domains_overlap(int a, int b)
{
    if (a == b)
        return 1;
    return (a == AF_INET && b == AF_INET6) || (a == AF_INET6 && b == AF_INET);
}

static int net_sockaddr_port_equal(const void *a, size_t alen,
                                   const void *b, size_t blen)
{
    uint16_t ap = 0;
    uint16_t bp = 0;
    return net_sockaddr_port(a, alen, &ap) == 0 &&
           net_sockaddr_port(b, blen, &bp) == 0 &&
           ap == bp;
}

static int net_sockaddr_is_local_target(const void *addr, size_t len)
{
    if (!addr || len < sizeof(net_sockaddr_in_t))
        return 0;
    int family = *(const uint16_t *)addr;
    if (family == AF_INET)
        return net_sockaddr_in_local((const net_sockaddr_in_t *)addr);
    if (family == AF_INET6 && len >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        int all_zero = 1;
        for (size_t i = 0; i < sizeof(in6->sin6_addr); i++) {
            if (in6->sin6_addr[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero)
            return 1;
        for (size_t i = 0; i < 15; i++) {
            if (in6->sin6_addr[i] != 0)
                return 0;
        }
        return in6->sin6_addr[15] == 1;
    }
    return 0;
}

/*
 * Both searches below walk the table one bucket at a time and hand back a
 * *referenced* socket.  The reference is what lets the caller drop the bucket
 * the search landed on before taking the pair of buckets it actually needs:
 * the bucket that owns the hit and the bucket that owns the requesting socket
 * are unrelated, and taking them in one critical section would mean nesting an
 * arbitrary shard under a held one.  Callers re-check in_registry under the
 * pair before using the result.
 *
 * Scanning in ascending slot order keeps the tie-breaks below (first match,
 * lowest accept_count) bit-identical to the pre-sharding whole-table scan.
 */
typedef struct {
    const net_socket_t *src;
    uint16_t            port;
    net_socket_t       *first;
    net_socket_t       *best;
    int                 best_load;
    int                 domain;
    /* src->local is snapshotted under src's own bucket before the scan: the
     * scan holds the *candidate's* bucket, which is generally not src's, so
     * reading src->local from inside the callback would be unlocked. */
    uint8_t             src_local[NET_SOCKADDR_MAX];
    size_t              src_local_len;
} net_inet_find_arg_t;

static bool net_find_stream_listener_slot(net_socket_t *cand, int idx,
                                          void *arg)
{
    (void)idx;
    net_inet_find_arg_t *a = (net_inet_find_arg_t *)arg;
    if (cand->net_ns != a->src->net_ns) return false;
    if (!cand->bound || !cand->listening || cand->type != SOCK_STREAM)
        return false;
    if (!net_inet_domains_overlap(cand->domain, a->domain))
        return false;
    uint16_t cand_port = 0;
    if (net_sockaddr_port(cand->local, cand->local_len, &cand_port) != 0 ||
        cand_port != a->port)
        return false;
    if (!a->first)
        a->first = net_socket_ref(cand);
    if (cand->accept_count < NET_MAX_QUEUE &&
        cand->accept_count < a->best_load) {
        net_socket_free(a->best);
        a->best = net_socket_ref(cand);
        a->best_load = cand->accept_count;
    }
    return false;
}

/* Returns a referenced listener, or NULL.  The caller frees it. */
static net_socket_t *net_find_stream_listener(net_socket_t *s, uint16_t port)
{
    net_inet_find_arg_t a;
    memset(&a, 0, sizeof(a));
    a.src = s;
    a.port = port;
    a.best_load = NET_MAX_QUEUE + 1;
    a.domain = s->domain;
    net_table_scan_all(net_find_stream_listener_slot, &a);
    if (a.best) {
        net_socket_free(a.first);
        return a.best;
    }
    return a.first;
}

static bool net_find_udp_dst_slot(net_socket_t *cand, int idx, void *arg)
{
    (void)idx;
    net_inet_find_arg_t *a = (net_inet_find_arg_t *)arg;
    if (cand->net_ns != a->src->net_ns) return false;
    if (cand == a->src || !cand->bound || cand->type != SOCK_DGRAM)
        return false;
    if (!net_inet_domains_overlap(cand->domain, a->domain))
        return false;
    uint16_t cand_port = 0;
    if (net_sockaddr_port(cand->local, cand->local_len, &cand_port) != 0 ||
        cand_port != a->port)
        return false;
    if (cand->connected) {
        if (net_sockaddr_port_equal(cand->peer_addr, cand->peer_len,
                                    a->src_local, a->src_local_len)) {
            /* A connected match wins outright and ends the scan; the previous
             * whole-table version returned it immediately too. */
            a->best = net_socket_ref(cand);
            return true;
        }
        return false;
    }
    if (!a->first)
        a->first = net_socket_ref(cand);
    return false;
}

/* Returns a referenced destination socket, or NULL.  The caller frees it. */
static net_socket_t *net_find_udp_dst(net_socket_t *src,
                                      const void *dst_addr, size_t dst_len)
{
    if (!src || !dst_addr)
        return NULL;
    uint16_t dst_port = 0;
    if (net_sockaddr_port(dst_addr, dst_len, &dst_port) < 0)
        return NULL;

    net_inet_find_arg_t a;
    memset(&a, 0, sizeof(a));
    a.src = src;
    a.port = dst_port;
    a.domain = src->domain;
    {
        uint64_t sf = net_sock_lock(src);
        a.src_local_len = src->local_len;
        memcpy(a.src_local, src->local, a.src_local_len);
        net_sock_unlock(src, sf);
    }
    net_table_scan_all(net_find_udp_dst_slot, &a);
    if (a.best)
        net_socket_free(a.first);
    return a.best ? a.best : a.first;
}

/*
 * Both families, because lwIP's tcp_bind()/tcp_connect()/udp_bind() take an
 * ip_addr_t that carries its own type and there is nothing downstream that
 * needs an IPv4-only view.  This used to refuse AF_INET6 outright, which is
 * what left net_inet_bind_pcb() and the whole stream connect path unreachable
 * for v6 -- a v6 socket could be bound in the socket layer and then had no pcb
 * behind it at all.
 *
 * AF_UNIX / AF_PACKET / AF_NETLINK still return -EOPNOTSUPP: they have no IP
 * representation, and mapping them onto the wildcard would silently bind a
 * socket to every address.
 */
int net_sockaddr_to_lwip_ip(const void *addr, size_t len,
                            ip_addr_t *ip, uint16_t *port)
{
    if (!addr || !ip || len < sizeof(net_sockaddr_in_t))
        return -EINVAL;
    const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)addr;
    if (in->sin_family == AF_INET) {
        ip_addr_set_ip4_u32(ip, in->sin_addr);
        if (port)
            *port = net_ntohs(in->sin_port);
        return 0;
    }
#if LWIP_IPV6
    if (in->sin_family == AF_INET6) {
        if (len < sizeof(net_sockaddr_in6_t))
            return -EINVAL;
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        int all_zero = 1;
        for (size_t i = 0; i < sizeof(in6->sin6_addr); i++) {
            if (in6->sin6_addr[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            /* `::` is the dual-stack wildcard that a LISTEN on AF_INET6 gets
             * by default (Go binds every wildcard listener this way, and
             * Linux's IPV6_V6ONLY defaults to 0).  lwIP only accepts an IPv4
             * packet for a listener whose local address has type
             * IPADDR_TYPE_ANY; an IPV6-typed zero address matches IPv6 alone,
             * so an IPv4 SYN to the tailnet address was answered with RST. */
            ip_addr_set_zero(ip);
            IP_SET_TYPE(ip, IPADDR_TYPE_ANY);
        } else {
            ip6_addr_t a6;
            memcpy(a6.addr, in6->sin6_addr, sizeof(a6.addr));
            a6.zone = 0;
            ip_addr_copy_from_ip6(*ip, a6);
        }
        if (port)
            *port = net_ntohs(in6->sin6_port);
        return 0;
    }
#endif
    return -EOPNOTSUPP;
}

/*
 * Lane for a bound socket address.  Returns `fallback` unchanged for anything
 * that is not IP -- AF_UNIX, AF_PACKET, AF_NETLINK and AF_ALG have no port to
 * key on and no PCB that an inbound packet has to find, so they stay where they
 * were provisionally placed.
 *
 * Both families are handled here rather than by reusing net_sockaddr_to_lwip_ip(),
 * which rejects IPv6.  For IPv6 the hash takes the low 32 bits of the address:
 * that is enough entropy to spread connections, and -- the part that actually
 * matters -- it is computed only from bytes that arrive on the wire, so the
 * peer's view and ours produce the same value.
 *
 * THE ANSWER IS ALWAYS A REAL LANE, INCLUDING FOR A WILDCARD BIND.  bind(0.0.0.0)
 * hashes the zero address, so this returns net_lane_of(0, port) -- a number in
 * 0..CONFIG_NET_LANES-1, never a sentinel.  That is deliberate and it is the
 * same number lwIP computes for the pcb: see NET_PCB_LANE_OWNER_OF_PCB() in
 * lwip/priv/pcb_lane.h, which answers "which lane owns this pcb's work" and is
 * distinct from NET_PCB_LANE_OF_PCB(), which answers "which list head does a
 * lookup walk" and returns the sentinel bucket for the same wildcard bind.
 * Stage D dispatches packets to the owning lane, so the socket side of the pair
 * has to be the owning one; keeping the wildcard case here on the owning
 * definition is what stops "socket says lane 2, pcb is somewhere else" from
 * becoming a real dispatch bug.
 */
unsigned net_socket_lane_of_addr(const void *addr, size_t len,
                                 unsigned fallback)
{
    if (!addr)
        return fallback;
    if (len >= sizeof(net_sockaddr_in_t)) {
        const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)addr;
        if (in->sin_family == AF_INET)
            return net_lane_of(in->sin_addr, net_ntohs(in->sin_port));
    }
    if (len >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        if (in6->sin6_family == AF_INET6) {
            uint32_t low;
            memcpy(&low, in6->sin6_addr + 12, sizeof(low));
            return net_lane_of(low, net_ntohs(in6->sin6_port));
        }
    }
    return fallback;
}


/*
 * Inverse of net_sockaddr_to_lwip_ip().  Dual-stack for the same reason: the
 * accept stage fills the child's peer address from the accepted pcb's remote
 * IP, and a v6 listener's accepted connection has an ip6_addr_t there.  With
 * the v4-only version that call returned -EINVAL and left peer_len at whatever
 * the previous stage left, so accept() on a v6 socket reported a v4-shaped
 * (or empty) peer.
 */
int net_lwip_ip_to_sockaddr(const ip_addr_t *ip, uint16_t port,
                            uint8_t out[NET_SOCKADDR_MAX], size_t *outlen)
{
    if (!out || !outlen || !ip)
        return -EINVAL;
    if (IP_IS_V4(ip)) {
        net_sockaddr_in_t in;
        memset(&in, 0, sizeof(in));
        in.sin_family = AF_INET;
        in.sin_port = net_htons(port);
        in.sin_addr = ip_2_ip4(ip)->addr;
        memcpy(out, &in, sizeof(in));
        *outlen = sizeof(in);
        return 0;
    }
#if LWIP_IPV6
    if (IP_IS_V6(ip)) {
        net_sockaddr_in6_t in6;
        memset(&in6, 0, sizeof(in6));
        in6.sin6_family = AF_INET6;
        in6.sin6_port = net_htons(port);
        memcpy(in6.sin6_addr, ip_2_ip6(ip)->addr,
               sizeof(in6.sin6_addr));
        memcpy(out, &in6, sizeof(in6));
        *outlen = sizeof(in6);
        return 0;
    }
#endif
    return -EINVAL;
}

/*
 * Bottom-half ring helpers.
 *
 * The ring is single-producer (lwIP callback running under g_lwip_lock,
 * interrupts disabled) and single-consumer (net_inet_bottom_half_process_socket
 * running under the socket's own lock only).  All index updates use __atomic
 * intrinsics so the ring is safe on SMP without holding both locks at once.
 *
 * "Single-consumer" still holds per socket: a slot index belongs to exactly one
 * bucket, and the per-socket bottom-half work runs with that bucket held, so
 * two CPUs can process two sockets in different buckets at the same time but
 * can never process the same socket twice.
 */
void net_inet_tcp_apply_options(net_socket_t *s, struct tcp_pcb *pcb);


#include "net/socket_inet_bh_state.h"

/*
 * The bottom-half event ring and its per-socket consumer are Rust
 * (kernel/net/backends/lwip/socket_inet_bh.rs).  What is left here is the ring's storage and
 * its index arithmetic, the event fields, the pbuf copy/ref/free, the socket
 * address and pktinfo fill, the receive-queue enqueue, the wait-queue
 * collect/wake, the event notify, the socket lock, the registry slot mark and
 * the perf counters.
 *
 * Rust owns the reservation and chunking policy, the spill-versus-copy choice,
 * what a full ring costs in each callback, the consumer's flag ordering (the
 * ring is drained against the pre-existing local close and the peer's FIN and
 * reset are applied only afterwards), the enqueue choice per event and the
 * wake-batch fallback.
 */

/* The accept drain is defined further down; the bottom half calls it after
 * unlocking, so the forward declaration is enough. */
static bool net_inet_accept_stage_drain(net_socket_t *listener,
                                        proc_wake_q_t *wake_q);

int a20_net_bh_reserve(void *sp, uint32_t n)
{
    net_socket_t *s = (net_socket_t *)sp;
    net_bh_ring_t *r = &lw(s)->bh_ring;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return ((head - tail) + n) <= NET_BH_RING_SIZE ? 1 : 0;
}

int a20_net_bh_prepare(void *sp, void **event_out)
{
    net_socket_t *s = (net_socket_t *)sp;
    net_bh_ring_t *r = &lw(s)->bh_ring;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if ((head - tail) >= NET_BH_RING_SIZE)
        return -1;
    uint32_t idx = head & (NET_BH_RING_SIZE - 1);
    if (r->owned[idx]) {
        pbuf_free(r->owned[idx]);
        r->owned[idx] = NULL;
    }
    net_bh_event_t *e = &r->events[idx];
    /* Header only: the inline payload is about to be overwritten up to e->len
     * and is never read past that. */
    memset(e, 0, __builtin_offsetof(net_bh_event_t, data));
    e->type = NET_BH_RECV;
    *event_out = e;
    return (int)idx;
}

void a20_net_bh_commit(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_fetch_add(&lw(s)->bh_ring.head, 1, __ATOMIC_RELAXED);
}

void *a20_net_bh_consume(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    net_bh_ring_t *r = &lw(s)->bh_ring;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    if (head == tail)
        return NULL;
    a20_perf_count(A20_PERF_NET_BH_EVENTS);
    return &r->events[tail & (NET_BH_RING_SIZE - 1)];
}

void a20_net_bh_consume_commit(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __atomic_fetch_add(&lw(s)->bh_ring.tail, 1, __ATOMIC_RELEASE);
}

/* Copying a spill into the socket queue completes before tail is published.
 * Reclaim those references outside the socket lock, under the producer's
 * lwIP lane lock. Keeping a ring's worth of large TCP chains until wrap can
 * otherwise pin the entire receive pbuf pool and stop further input. */
void a20_net_bh_reclaim_spills(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    net_bh_ring_t *r = &lw(s)->bh_ring;
    uint64_t flags = net_inet_socket_lane_lock(s);
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t live = head - tail;
    for (uint32_t i = 0; i < NET_BH_RING_SIZE; i++) {
        uint32_t distance = (i - tail) & (NET_BH_RING_SIZE - 1);
        if (distance >= live && r->owned[i]) {
            pbuf_free(r->owned[i]);
            r->owned[i] = NULL;
        }
    }
    a20_lwip_lane_unlock(flags);
}

void a20_net_bh_stage_payload(void *sp, void *event, void *pp, uint32_t off,
                              size_t len)
{
    net_socket_t *s = (net_socket_t *)sp;
    net_bh_ring_t *r = &lw(s)->bh_ring;
    net_bh_event_t *e = (net_bh_event_t *)event;
    struct pbuf *p = (struct pbuf *)pp;
    if (len <= NET_BH_INLINE_PAYLOAD) {
        pbuf_copy_partial(p, e->data, (u16_t)len, (u16_t)off);
        e->spill = NULL;
        e->spill_off = 0;
        e->len = len;
        return;
    }
    /* Large datagrams and coalesced TCP receive chains are held by reference. */
    pbuf_ref(p);
    r->owned[((uintptr_t)e - (uintptr_t)r->events) /
             sizeof(net_bh_event_t)] = p;
    e->spill = p;
    e->spill_off = off;
    e->len = len;
}

uint32_t a20_net_bh_inline_payload(void)
{
    return (uint32_t)NET_BH_INLINE_PAYLOAD;
}

int a20_net_bh_max_sockets(void)
{
    return NET_MAX_SOCKETS;
}

int a20_net_bh_accept_pending(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    return (s->listening && lw(s)->accept_stage.head != lw(s)->accept_stage.tail) ? 1 : 0;
}

void a20_net_bh_publish_pending(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    __atomic_store_n(&s->bh_pending, 1, __ATOMIC_RELEASE);
    net_bh_slot_mark(s->reg_idx);
}

void a20_net_bh_slot_clear(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    net_bh_slot_clear(s->reg_idx);
}

uint32_t a20_net_bh_pbuf_len(void *p)
{
    return ((struct pbuf *)p)->tot_len;
}

void a20_net_bh_pbuf_free(void *p)
{
    pbuf_free((struct pbuf *)p);
}

void a20_net_bh_addr_from_ip(void *event, const void *ip, uint16_t port)
{
    net_bh_event_t *e = (net_bh_event_t *)event;
    net_lwip_ip_to_sockaddr((const ip_addr_t *)ip, port, e->addr, &e->addrlen);
}

void a20_net_bh_pktinfo(void *event)
{
    net_bh_event_t *e = (net_bh_event_t *)event;
    if (!ip_current_is_v6()) {
        e->has_pktinfo = 1;
        e->pktinfo_family = AF_INET;
        struct netif *input = ip_current_input_netif();
        e->pktinfo_ifindex = input ? netif_get_index(input) : 0;
        memset(e->pktinfo_addr,0,sizeof(e->pktinfo_addr));
        memcpy(e->pktinfo.ipv4.destination,ip4_current_dest_addr(),4);
        if (input) memcpy(e->pktinfo.ipv4.local,netif_ip4_addr(input),4);
        return;
    }
#if LWIP_IPV6
    if (ip_current_is_v6()) {
        e->pktinfo_family = AF_INET6;
        e->has_pktinfo = 1;
        e->pktinfo_ifindex = ip_current_input_netif() ?
            (uint32_t)netif_get_index(ip_current_input_netif()) : 0;
        memcpy(e->pktinfo_addr, ip6_current_dest_addr(), sizeof(e->pktinfo_addr));
        e->has_hoplimit = 1;
        e->hoplimit = IP6H_HOPLIM(ip6_current_header());
        e->has_tclass = 1;
        e->tclass = IP6H_TC(ip6_current_header());
    }
#else
    (void)e;
#endif
}

int a20_net_bh_raw_icmp_echo(void *sp, void *pp)
{
    net_socket_t *s = (net_socket_t *)sp;
    struct pbuf *p = (struct pbuf *)pp;
    /* Do not consume ICMP echo requests on raw ICMP sockets.  Let lwIP's
     * icmp_input() generate the echo reply so the socket can receive it; the
     * pbuf passed to the raw recv callback still contains the IP header. */
    if (s->protocol == IPPROTO_ICMP && p->tot_len > 0) {
        struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
        u16_t iphdr_hlen = IPH_HL_BYTES(iphdr);
        if (p->tot_len > iphdr_hlen) {
            uint8_t *payload = (uint8_t *)p->payload;
            if (payload[iphdr_hlen] == ICMP_ECHO)
                return 1;
        }
    }
    return 0;
}

static net_recv_meta_t event_metadata(const net_bh_event_t *event) {
    net_recv_meta_t meta = {0};
    meta.has_pktinfo = event->has_pktinfo;
    meta.has_hoplimit = event->has_hoplimit;
    meta.has_tclass = event->has_tclass;
    meta.pktinfo_family = event->pktinfo_family;
    meta.pktinfo_ifindex = event->pktinfo_ifindex;
    meta.pktinfo = event->pktinfo;
    meta.hoplimit = event->hoplimit;
    meta.tclass = event->tclass;
    return meta;
}
static int read_pbuf(const void *source, void *out, size_t len, uint32_t offset) {
    const struct pbuf *p = source;
    if (offset > p->tot_len || len > p->tot_len - offset) return -EMSGSIZE;
    return pbuf_copy_partial(p, out, (u16_t)len, (u16_t)offset) == len ? 0 : -EIO;
}
int a20_net_bh_enqueue_spill(void *sp, void *event) {
    net_bh_event_t *e = event;
    net_recv_meta_t meta = event_metadata(e);
    return net_enqueue_msg_locked_reader(sp, e->spill, e->spill_off, e->len,
        e->addrlen ? e->addr : NULL, e->addrlen, &meta, read_pbuf);
}
int a20_net_bh_enqueue_inline(void *sp, void *event) {
    net_bh_event_t *e = event;
    net_recv_meta_t meta = event_metadata(e);
    return net_enqueue_msg_locked_meta(sp, e->data, e->len,
        e->addrlen ? e->addr : NULL, e->addrlen, &meta);
}

int a20_net_bh_collect_all(void *sp, int which, void *wake_q)
{
    net_socket_t *s = (net_socket_t *)sp;
    wait_queue_t *q = which ? &s->write_waitq : &s->read_waitq;
    return net_wait_queue_collect_all_locked(q, PROC_WAKE_EVENT,
                                             (proc_wake_q_t *)wake_q) ? 1 : 0;
}

int a20_net_bh_collect_one(void *sp, void *wake_q)
{
    net_socket_t *s = (net_socket_t *)sp;
    return (int)wait_queue_collect_one(&s->read_waitq, 0, PROC_WAKE_EVENT,
                                       (proc_wake_q_t *)wake_q);
}

int a20_net_bh_wakeq_count(void *wake_q)
{
    return (int)((proc_wake_q_t *)wake_q)->count;
}

void a20_net_bh_notify(void *sp, uint32_t event, uint64_t d0, uint64_t d1)
{
    net_socket_t *s = (net_socket_t *)sp;
    if (event == A20_EVENT_ERROR) {
        /* lwIP error numbers are private to the transport. SO_ERROR reports
         * a positive public errno and consumes it independently of tcp_err,
         * which the blocking connect transaction still needs. */
        int error = (int)d0;
        switch (error) {
        case ERR_OK: error = 0; break;
        case ERR_MEM: case ERR_BUF: error = ENOMEM; break;
        case ERR_TIMEOUT: error = ETIMEDOUT; break;
        case ERR_RTE: error = ENETUNREACH; break;
        case ERR_USE: error = EADDRINUSE; break;
        case ERR_RST: error = s->ever_connected ? ECONNRESET : ECONNREFUSED; break;
        case ERR_ABRT: error = ECONNABORTED; break;
        case ERR_CLSD: case ERR_CONN: error = ENOTCONN; break;
        default: error = EIO; break;
        }
        s->pending_error = error;
    }
    net_event_notify(s, event, d0, d1);
}

void a20_net_bh_socket_lock(void *sp, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)sp);
}

void a20_net_bh_socket_unlock(void *sp, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)sp, flags);
}

int a20_net_bh_is_live(void *sp)
{
    return net_socket_is_live((const net_socket_t *)sp) ? 1 : 0;
}

void a20_net_bh_socket_ref(void *sp)
{
    net_socket_ref((net_socket_t *)sp);
}

void a20_net_bh_socket_free(void *sp)
{
    net_socket_free((net_socket_t *)sp);
}

void a20_net_bh_perf(int which)
{
    if (which == A20_NET_BH_PERF_OVERFLOW)
        a20_perf_count(A20_PERF_NET_BH_OVERFLOW);
    else if (which == A20_NET_BH_PERF_RUNS)
        a20_perf_count(A20_PERF_NET_BH_RUNS);
}

int a20_net_bh_accept_drain(void *sp, void *wake_q)
{
    return net_inet_accept_stage_drain((net_socket_t *)sp,
                                       (proc_wake_q_t *)wake_q) ? 1 : 0;
}

void a20_net_bh_wakeq_init(void *wake_q)
{
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void a20_net_bh_wakeq_flush(void *wake_q)
{
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

void a20_net_bh_wake_all(void *sp, int which)
{
    net_socket_t *s = (net_socket_t *)sp;
    if (which == 2)
        (void)wait_queue_wake_all(&s->accept_waitq, 0, PROC_WAKE_EVENT);
    else if (which == 1)
        (void)wait_queue_wake_all(&s->write_waitq, 0, PROC_WAKE_EVENT);
    else
        (void)wait_queue_wake_all(&s->read_waitq, 0, PROC_WAKE_EVENT);
}

void a20_net_bh_event_view(void *event, net_bh_event_view_t *out)
{
    net_bh_event_t *e = (net_bh_event_t *)event;
    out->event = e;
    out->type = (int *)&e->type;
    out->err = &e->err;
    out->len = &e->len;
    out->addr = e->addr;
    out->addrlen = &e->addrlen;
    out->has_pktinfo = &e->has_pktinfo;
    out->has_hoplimit = &e->has_hoplimit;
    out->has_tclass = &e->has_tclass;
    out->pktinfo_ifindex = &e->pktinfo_ifindex;
    out->pktinfo_addr = e->pktinfo_addr;
    out->hoplimit = &e->hoplimit;
    out->tclass = &e->tclass;
    out->spill = (void **)&e->spill;
    out->spill_off = &e->spill_off;
}

void a20_net_bh_socket_view_init(void *sp, net_bh_socket_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->closed = &s->closed;
    out->peer_closed = &s->peer_closed;
    out->tcp_connecting = &s->tcp_connecting;
    out->tcp_err = &s->tcp_err;
    out->connected = &s->connected;
    out->ever_connected = &s->ever_connected;
    out->listening = &s->listening;
    out->reg_idx = &s->reg_idx;
    out->bh_connected = &s->bh_connected;
    out->bh_closed = &s->bh_closed;
    out->bh_error = &s->bh_error;
    out->bh_err_code = &s->bh_err_code;
    out->bh_tx_wake = &s->bh_tx_wake;
    out->bh_pending = &s->bh_pending;
}

static void net_inet_bh_schedule(net_socket_t *s)
{
    (void)a20_net_bh_schedule(s);
}

/* The accept drain stages a pipelined payload into the child's ring through the
 * same all-or-nothing path the receive callbacks use. */
static bool net_inet_tcp_stage_payload(net_socket_t *s, struct pbuf *p)
{
    return a20_net_bh_stage_tcp(s, p) != 0;
}

/* Individual TCP segments fit inline; lwIP's coalesced receive chains use the
 * referenced-pbuf spill arm, which also serves large datagrams. */
_Static_assert(NET_BH_INLINE_PAYLOAD >= TCP_MSS,
               "inline bottom-half staging must cover a full TCP segment");
_Static_assert(NET_BH_INLINE_PAYLOAD < NET_MAX_PAYLOAD,
               "the inline staging area must be smaller than the payload cap");


static void lwip_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return;
    /* The gate, the pktinfo fill, the length clamp, the spill choice and the
     * drop all live in a20_net_bh_udp_recv(); this only keeps the callback. */
    a20_net_bh_udp_recv(s, p, addr, port);
}


static u8_t lwip_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return 0;
    /* The echo-request filter, the ring gate and the ownership rule for a
     * dropped frame are a20_net_bh_raw_recv()'s. */
    return (u8_t)a20_net_bh_raw_recv(s, p, addr);
}

static err_t lwip_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;

    __atomic_store_n(&s->bh_err_code, (int)err, __ATOMIC_RELEASE);
    __atomic_store_n(&s->bh_connected, 1, __ATOMIC_RELEASE);
    net_inet_bh_schedule(s);
    return ERR_OK;
}


static err_t lwip_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                              err_t err)
{
    (void)pcb;
    (void)err;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;
    /* A null pbuf is a FIN, recorded as the peer's half-close; a full ring
     * returns ERR_MEM and leaves the pbuf with lwIP. */
    return a20_net_bh_tcp_recv(s, p) == 0 ? ERR_OK : ERR_MEM;
}

static void lwip_tcp_err_cb(void *arg, err_t err)
{
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return;
    lw(s)->tcp = NULL;
    __atomic_store_n(&s->bh_err_code, (int)err, __ATOMIC_RELEASE);
    __atomic_store_n(&s->bh_error, 1, __ATOMIC_RELEASE);
    net_inet_bh_schedule(s);
}

static err_t lwip_tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;
    __atomic_store_n(&s->bh_tx_wake, 1, __ATOMIC_RELEASE);
    net_inet_bh_schedule(s);
    return ERR_OK;
}

/*
 * Callbacks for a pcb parked in the accept stage.
 *
 * Returning ERR_OK from tcp_accept tells lwIP the application owns the pcb, but
 * ownership is not readiness: the bottom half that installs the socket's real
 * callbacks has not run yet, and a pcb sitting there with pcb->recv == NULL is
 * not safe to leave alone.  lwIP answers exactly that situation with
 * tcp_recv_null(), whose "a FIN arrived" branch calls tcp_close(), and the
 * CLOSE_WAIT purge frees the pcb outright -- while the stage still points at
 * it.  The stage then hands the freed pointer to a fresh child socket and
 * teardown aborts it a second time, which is a memp double free, not a leak.
 *
 * So the socket layer services a staged pcb itself from the moment it is
 * staged.  These callbacks are what "itself" means while there is no socket
 * yet, and they cover the three things that can happen in the meantime: more
 * data, a FIN, and lwIP tearing the pcb down on its own.
 *
 * They run with g_lwip_lock held and interrupts off, under the same contract as
 * the accept callback: no allocation, no net lock, no scheduler.
 */
static err_t lwip_tcp_stage_recv_cb(void *arg, struct tcp_pcb *pcb,
                                    struct pbuf *p, err_t err)
{
    net_accept_stage_slot_t *c = (net_accept_stage_slot_t *)arg;
    if (!c) {
        if (p)
            pbuf_free(p);
        return ERR_OK;
    }

    if (!p) {
        /* A FIN, not a teardown.  The connection is still ours to hand to
         * accept(), so record it for the child instead of letting lwIP close
         * the pcb out from under the stage.  `dead` is deliberately not set
         * here: only lwip_tcp_stage_err_cb may claim the pcb is gone, because
         * it is the only one of the two that runs after lwIP has actually
         * freed it, and acting on a stale pointer is the whole failure this
         * exists to prevent. */
        c->fin = 1;
        c->error = (int)err;
        if (c->listener)
            net_inet_bh_schedule(c->listener);
        return ERR_OK;
    }

    if (c->pcb != pcb) {
        /* The slot was recycled while this pcb still pointed at it.  That can
         * only mean the bottom half already handed the pcb to its child and
         * re-argued it, so this event belongs to the child now and there is
         * nothing left to record against the slot. */
        pbuf_free(p);
        return ERR_OK;
    }

    /* Keep the payload instead of dropping it.  A peer may send in the same
     * segment that finalises the handshake, and accept() has not run yet, so
     * there is no socket for it to belong to; tcp_recv_null() used to free it
     * outright, losing whatever the client had already sent. */
    if (c->pending)
        pbuf_cat(c->pending, p);   /* takes the references the stage needs */
    else
        c->pending = p;            /* ownership came with the callback */
    if (c->listener)
        net_inet_bh_schedule(c->listener);
    return ERR_OK;
}

static err_t lwip_tcp_stage_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg;
    (void)pcb;
    (void)len;
    /* Nothing is writing from the peer yet, and nobody is waiting on the
     * window -- the child socket gets the real sent callback at adoption. */
    return ERR_OK;
}

static void lwip_tcp_stage_err_cb(void *arg, err_t err)
{
    net_accept_stage_slot_t *c = (net_accept_stage_slot_t *)arg;
    if (!c)
        return;
    c->dead = 1;
    c->error = (int)err;
    if (c->pcb) {
        /* Our pbuf references have to go now, while g_lwip_lock -- the only
         * context memp may be touched in -- is still held. */
        if (c->pending) {
            pbuf_free(c->pending);
            c->pending = NULL;
        }
        c->pcb = NULL;
    }
    if (c->listener)
        net_inet_bh_schedule(c->listener);
}

/*
 * A handshake completed on a real lwIP listening socket.
 *
 * Runs with the child PCB's lane held.  The listener may receive accepts from
 * several input lanes, so only publication into its staging ring uses the
 * listener-keyed shared guard.  Returning an error leaves aborting the child
 * to tcp_process(), after this callback and its shared guard have returned.
 */

#include "net/socket_inet_accept_state.h"

/*
 * The staged-accept state machine is Rust (kernel/net/backends/lwip/socket_inet_accept.rs).
 * What is left here is the primitive set it calls, the lwIP callback that
 * publishes into the ring, and the wrappers that keep the names the rest of this
 * file calls.
 *
 * Rust owns the ring's full/empty decision and the drop counter, the publication
 * order (the pcb is serviced before the slot is published, so the slot address
 * can be the pcb's callback_arg), the single-consumer claim, the backlog-full
 * refusal, the adoption order, the rollback when the push fails, the `dead`
 * handling that must neither adopt nor abort, the tail publication, and the
 * purge walk.
 *
 * C owns the primitives: the listener-keyed shared guard the producer takes, the
 * lwIP lock and the lane entry, the pcb callback installation and abort, the
 * pbuf walk and free, the child's socket alloc/free and namespace reference, the
 * option and peer-address inheritance, the payload staging into the child's
 * bottom-half ring, the registry register/unregister, the accept-queue push, the
 * event notify and wake-queue collect, the bottom-half schedule, the perf
 * counters and the socket lock.
 */

void a20_net_accept_view_init(void *sp, net_accept_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = (void **)&s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->protocol = &s->protocol;
    out->closed = &s->closed;
    out->listening = &s->listening;
    out->nonblock = &s->nonblock;
    out->bound = &s->bound;
    out->connected = &s->connected;
    out->ever_connected = &s->ever_connected;
    out->accept_count = &s->accept_count;
    out->tcp_nodelay = &s->tcp_nodelay;
    out->tcp_congestion = &s->tcp_congestion;
    out->snd_buf = &s->snd_buf;
    out->rcv_buf = &s->rcv_buf;
    out->keepalive = &s->keepalive;
    out->keep_idle = &s->keep_idle;
    out->keep_intvl = &s->keep_intvl;
    out->keep_cnt = &s->keep_cnt;
    out->recv_timeout_ticks = &s->recv_timeout_ticks;
    out->send_timeout_ticks = &s->send_timeout_ticks;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->tcp = (void **)&lw(s)->tcp;
    out->peer_closed = &s->peer_closed;
    out->bh_closed = &s->bh_closed;
    out->slots = lw(s)->accept_stage.slots;
    out->head = &lw(s)->accept_stage.head;
    out->tail = &lw(s)->accept_stage.tail;
    out->dropped = &lw(s)->accept_stage.dropped;
    out->drain_active = &lw(s)->accept_stage.drain_active;
}

uint64_t a20_net_accept_stage_lock(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    return a20_lwip_shared_lock(A20_LWIP_SHARED_LISTENER, lw(s)->tcp);
}

void a20_net_accept_stage_unlock(void *sp, uint64_t flags)
{
    net_socket_t *s = (net_socket_t *)sp;
    a20_lwip_shared_unlock(A20_LWIP_SHARED_LISTENER, lw(s)->tcp, flags);
}

void a20_net_accept_sock_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_accept_sock_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

uint64_t a20_net_accept_lwip_lock(void)
{
    return a20_lwip_lock();
}

void a20_net_accept_lwip_unlock(uint64_t flags)
{
    a20_lwip_unlock(flags);
}

int a20_net_accept_is_live(void *s)
{
    return net_socket_is_live((const net_socket_t *)s) ? 1 : 0;
}

void a20_net_accept_slot_arm(void *slotp, void *pcbp)
{
    net_accept_stage_slot_t *c = (net_accept_stage_slot_t *)slotp;
    struct tcp_pcb *pcb = (struct tcp_pcb *)pcbp;
    tcp_arg(pcb, c);
    tcp_recv(pcb, lwip_tcp_stage_recv_cb);
    tcp_err(pcb, lwip_tcp_stage_err_cb);
    tcp_sent(pcb, lwip_tcp_stage_sent_cb);
}

/* Release a slot's payload references and unbind it from its pcb.  The lwIP
 * lock must be held: these are memp-owned pbuf references and memp has no
 * internal locking.  `pcb` is left alone -- whether it still needs aborting is
 * the caller's decision, because lwIP may already have freed it. */
void a20_net_accept_slot_release(void *slotp)
{
    net_accept_stage_slot_t *c = (net_accept_stage_slot_t *)slotp;
    while (c->pending) {
        struct pbuf *p = c->pending;
        c->pending = p->next;
        p->next = NULL;
        pbuf_free(p);
    }
    c->pcb = NULL;
    c->listener = NULL;
    c->fin = 0;
    c->dead = 0;
    c->error = 0;
}

void *a20_net_accept_alloc(void)
{
    net_socket_t *s = net_socket_alloc();
    if (s && !net_inet_state_attach(s)) { net_socket_free(s); return NULL; }
    return s;
}

void a20_net_accept_free(void *s)
{
    net_socket_free((net_socket_t *)s);
}

void a20_net_accept_ns_put(void *ns)
{
    resource_ns_put((resource_namespace_t *)ns);
}

void *a20_net_accept_ns_get(void *ns)
{
    return resource_ns_get((resource_namespace_t *)ns);
}

/*
 * Declare the child's owning lane, taken from the pcb it is adopting rather than
 * from the listener: for a wildcard bind the two are not the same number.
 *
 * `child` may be NULL -- the backlog was full or the allocation failed -- and
 * the C version dereferenced it here, which is a NULL store at NET_LANES > 1.
 * With no child there is no lane to declare, so this enters the listener's,
 * which is what the one-lane branch does.
 */
void a20_net_accept_lane(void *childp, void *pcbp, void *listenerp)
{
#if CONFIG_NET_LANES > 1
    net_socket_t *child = (net_socket_t *)childp;
    if (child) {
        struct tcp_pcb *pcb = (struct tcp_pcb *)pcbp;
        net_socket_lane_store(child, pcb ? NET_PCB_LANE_OWNER_OF_PCB(pcb)
                                         : net_socket_lane_load(listenerp));
        a20_lwip_lane_enter(net_socket_lane_load(child));
        return;
    }
#else
    /* At one lane a20_lwip_lane_enter() discards its argument, so the load is
     * the only use of listenerp and the other two are unused by construction. */
    (void)childp;
    (void)pcbp;
    (void)listenerp;
    a20_lwip_lane_enter(net_socket_lane_load((net_socket_t *)listenerp));
#endif
}

void a20_net_accept_apply_options(void *child, void *pcb)
{
    net_inet_tcp_apply_options((net_socket_t *)child, (struct tcp_pcb *)pcb);
}

void a20_net_accept_peer_addr(void *childp, void *pcbp)
{
    net_socket_t *child = (net_socket_t *)childp;
    struct tcp_pcb *pcb = (struct tcp_pcb *)pcbp;
    net_lwip_ip_to_sockaddr(&pcb->remote_ip, 0, child->peer_addr,
                            &child->peer_len);
    net_sockaddr_set_port(child->peer_addr, child->peer_len,
                          (uint16_t)pcb->remote_port);
}

void *a20_net_accept_pbuf_next(void *p)
{
    return ((struct pbuf *)p)->next;
}

void a20_net_accept_pbuf_unlink(void *p)
{
    ((struct pbuf *)p)->next = NULL;
}

void a20_net_accept_pbuf_free(void *p)
{
    pbuf_free((struct pbuf *)p);
}

int a20_net_accept_stage_payload(void *child, void *p)
{
    return net_inet_tcp_stage_payload((net_socket_t *)child, (struct pbuf *)p)
               ? 1 : 0;
}

void a20_net_accept_pcb_abort(void *pcb)
{
    tcp_arg((struct tcp_pcb *)pcb, NULL);
    tcp_abort((struct tcp_pcb *)pcb);
}

void a20_net_accept_bh_closed(void *child)
{
    __atomic_store_n(&((net_socket_t *)child)->bh_closed, 1, __ATOMIC_RELEASE);
}

int a20_net_accept_register(void *child)
{
    return net_register_socket_locked((net_socket_t *)child);
}

int a20_net_accept_unregister(void *child)
{
    return net_socket_unregister((net_socket_t *)child) ? 1 : 0;
}

int a20_net_accept_queue_push(void *listener, void *child)
{
    return net_accept_queue_push_locked((net_socket_t *)listener,
                                        (net_socket_t *)child);
}

/* Abort a child's pcb and drop the pointer, under the lwIP lock. */
void a20_net_accept_abort_child(void *childp)
{
    net_socket_t *child = (net_socket_t *)childp;
    uint64_t cf = a20_lwip_lock();
    tcp_abort(lw(child)->tcp);
    lw(child)->tcp = NULL;
    a20_lwip_unlock(cf);
}

void a20_net_accept_notify_accept(void *listener, void *wake_q, int *woke)
{
    net_socket_t *l = (net_socket_t *)listener;
    net_event_notify(l, A20_EVENT_ACCEPT_READY, 0, 0);
    if (wait_queue_collect_one(&l->accept_waitq, 0, PROC_WAKE_EVENT,
                               (proc_wake_q_t *)wake_q))
        *woke = 1;
}

void a20_net_accept_bh_schedule(void *s)
{
    net_inet_bh_schedule((net_socket_t *)s);
}

void a20_net_accept_race_delay(void)
{
#if CONFIG_NET_RACE_DELAY_US
    /* Diagnostic amplifier; see net_profile.h.  Placed in the gap between the
     * dequeue and the lwIP lock acquisition on purpose -- that gap is the window
     * this exists to widen. */
    for (volatile uint32_t d = 0; d < CONFIG_NET_RACE_DELAY_US; d++)
        __asm__ __volatile__("" ::: "memory");
#endif
}

void a20_net_accept_perf(int which)
{
    switch (which) {
    case A20_NET_ACCEPT_PERF_STAGED:
        a20_perf_count(A20_PERF_NET_ACCEPT_STAGED);
        break;
    case A20_NET_ACCEPT_PERF_QUEUED:
        a20_perf_count(A20_PERF_NET_ACCEPT_QUEUED);
        break;
    case A20_NET_ACCEPT_PERF_DROP:
        a20_perf_count(A20_PERF_NET_ACCEPT_DROP);
        break;
    case A20_NET_ACCEPT_PERF_ALLOC:
        a20_perf_count(A20_PERF_NET_ALLOC_FAIL);
        break;
    case A20_NET_ACCEPT_PERF_OVERFLOW:
        a20_perf_count(A20_PERF_NET_BH_OVERFLOW);
        break;
    default:
        break;
    }
}

/*
 * A handshake completed on a real lwIP listening socket.  Runs with the child
 * pcb's lane held; only the publication into the listener's ring is Rust's.
 */
static err_t lwip_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    net_socket_t *s = (net_socket_t *)arg;
    if (!newpcb)
        return ERR_OK;
    if (!s || err != ERR_OK)
        return ERR_MEM;
    return a20_net_accept_publish(s, newpcb, NET_ACCEPT_STAGE_SIZE) == 0
               ? ERR_OK : ERR_MEM;
}

/* Tear down every pcb still parked in a listener's accept stage.  The lwIP lock
 * is held, which also means the producer is not running. */
static void net_inet_accept_stage_purge(net_socket_t *s)
{
    a20_net_accept_purge(s, NET_ACCEPT_STAGE_SIZE);
}

static bool net_inet_accept_stage_drain(net_socket_t *listener,
                                        proc_wake_q_t *wake_q)
{
    return a20_net_accept_drain(listener, wake_q, NET_ACCEPT_STAGE_SIZE,
                                NET_MAX_QUEUE) != 0;
}

/*
 * True when lw(s)->tcp is a listening pcb rather than a connection.
 *
 * tcp_recv/tcp_sent/tcp_err all assert pcb->state != LISTEN, so every teardown
 * path has to ask the pcb what it is instead of assuming lw(s)->tcp is an
 * established connection.  A real LISTEN pcb exists whenever tcpmode is lwip;
 * before that, net_listen() dropped the bound pcb and lw(s)->tcp was always an
 * ordinary pcb, which is why this could be written as an unconditional call.
 */
static bool net_inet_tcp_pcb_is_listen(const net_socket_t *s)
{
    return lw(s)->tcp && lw(s)->tcp->state == LISTEN;
}

void a20_lwip_transport_close_pcb(net_socket_t *s)
{
    if (!s) return;
    uint64_t flags = a20_lwip_lock();
    if (!lw(s)->tcp) { a20_lwip_unlock(flags); return; }
    a20_lwip_lane_enter(net_socket_lane_load(s));
    tcp_arg(lw(s)->tcp, NULL);
    if (net_inet_tcp_pcb_is_listen(s)) {
        tcp_accept(lw(s)->tcp, NULL);
        tcp_close(lw(s)->tcp);
        net_inet_accept_stage_purge(s);
    } else {
        tcp_recv(lw(s)->tcp, NULL);
        tcp_err(lw(s)->tcp, NULL);
        tcp_sent(lw(s)->tcp, NULL);
        if (tcp_close(lw(s)->tcp) != ERR_OK)
            tcp_abort(lw(s)->tcp);
    }
    lw(s)->tcp = NULL;
    a20_lwip_unlock(flags);
}

void a20_lwip_transport_drop_pcb(net_socket_t *s)
{
    if (!s) return;
    uint64_t flags = a20_lwip_lock();
    if (!lw(s)->tcp) { a20_lwip_unlock(flags); return; }
    a20_lwip_lane_enter(net_socket_lane_load(s));
    tcp_arg(lw(s)->tcp, NULL);
    if (net_inet_tcp_pcb_is_listen(s)) {
        tcp_close(lw(s)->tcp);
        net_inet_accept_stage_purge(s);
    } else {
        tcp_recv(lw(s)->tcp, NULL);
        tcp_err(lw(s)->tcp, NULL);
        tcp_sent(lw(s)->tcp, NULL);
        if (tcp_close(lw(s)->tcp) != ERR_OK)
            tcp_abort(lw(s)->tcp);
    }
    lw(s)->tcp = NULL;
    a20_lwip_unlock(flags);
}

/*
 * Process a single socket's deferred bottom-half work.
 *
 * Runs with that socket's own lock held and WITHOUT a20_lwip_lock.  Allocates
 * net_msg_t entries, copies staged payload data, and detaches waiters into
 * wake_q.  The caller flushes wake_q only after dropping the socket lock.
 *
 * The accept drain is deliberately NOT here: it registers each child through
 * net_register_socket_locked(), which takes a socket-table shard and must not
 * run under a held socket lock.  This function only reports through
 * `accept_pending` that a drain is outstanding; the callers run it after
 * unlocking and OR in NET_BH_DRAIN_ACCEPT with the drain's own result.
 */
#define NET_BH_DRAIN_READ  (1U << 0)
#define NET_BH_DRAIN_WRITE (1U << 1)
#define NET_BH_DRAIN_ACCEPT (1U << 2)


#include "net/socket_inet_bh_state.h"

_Static_assert(A20_NET_BH_DRAIN_READ == NET_BH_DRAIN_READ &&
               A20_NET_BH_DRAIN_WRITE == NET_BH_DRAIN_WRITE &&
               A20_NET_BH_DRAIN_ACCEPT == NET_BH_DRAIN_ACCEPT,
               "the Rust drain bits must match the C ones");

static unsigned
net_inet_bottom_half_process_socket_locked(net_socket_t *s,
                                           proc_wake_q_t *wake_q,
                                           bool *accept_pending)
{
    int ap = 0;
    unsigned drain = (unsigned)a20_net_bh_process_locked(s, wake_q, &ap);
    *accept_pending = ap != 0;
    return drain;
}

void net_inet_bottom_half_process_socket(net_socket_t *s)
{
    if (s && s->transport_private) a20_net_bh_process_socket(s);
}


void net_inet_bottom_half_process_all(void)
{
    /* One acquire load replaces a 1024-slot scan per context switch; a mark
     * racing after this check simply stays pending for the next switch.  The
     * read goes through the registry's own accessor so the flag transitions and
     * the flag reads cannot end up owned by different selectors. */
    if (!net_bh_any_pending())
        return;
    a20_perf_count(A20_PERF_NET_BH_RUNS);
    /* No bucket is held across the walk.  The pending bitmap is read without a
     * lock (it is a volatile int each producer sets atomically), a slot that is
     * actually pending is then re-fetched under its own bucket by
     * net_bucket_slot_ref(), and the work runs under that socket's lock.  The
     * old shape held one bucket across the whole inner loop, which is the exact
     * "take the whole shard set" pattern the lock rules forbid. */
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!net_bh_slot_pending(i))
            continue;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        unsigned drain = 0;
        bool accept_pending = false;
        net_socket_t *s = net_bucket_slot_ref(i);
        if (!s)
            continue;
        {
            /* The bucket lock is now taken only to reach the slot table and
             * take a reference; the bottom half itself runs under the socket's
             * own lock.  Before, one pending socket's wake drain held the whole
             * 512-slot bucket, so every other socket in it queued behind it. */
            uint64_t irq = net_sock_lock(s);
            /* Consume the notification before observing producer state. A
             * callback racing with the drain must leave a new mark behind;
             * clearing afterwards could strand the final receive forever. */
            __atomic_store_n(&s->bh_pending, 0, __ATOMIC_RELEASE);
            net_bh_slot_clear(i);
            if (net_socket_is_live(s)) {
                drain = net_inet_bottom_half_process_socket_locked(
                    s, &wake_q, &accept_pending);
                if (accept_pending)
                    net_socket_ref(s);
            }
            net_sock_unlock(s, irq);
        }
        a20_net_bh_reclaim_spills(s);
        /* Two references at most: the one net_bucket_slot_ref() took, plus the
         * one the accept drain below needs. */
        if (accept_pending && net_inet_accept_stage_drain(s, &wake_q))
            drain |= NET_BH_DRAIN_ACCEPT;
        if (accept_pending)
            net_socket_free(s);
        (void)proc_wake_q_flush(&wake_q);
        if (drain & NET_BH_DRAIN_READ)
            (void)wait_queue_wake_all(
                &s->read_waitq, 0, PROC_WAKE_EVENT);
        if (drain & NET_BH_DRAIN_WRITE)
            (void)wait_queue_wake_all(
                &s->write_waitq, 0, PROC_WAKE_EVENT);
        if (drain & NET_BH_DRAIN_ACCEPT)
            (void)wait_queue_wake_all(
                &s->accept_waitq, 0, PROC_WAKE_EVENT);
        /* Wait queues belong to s; retain our walk reference through wakeup. */
        net_socket_free(s);
    }
}

/*
 * Select the congestion control algorithm for a pcb.
 *
 * Separate from net_inet_tcp_apply_options() because setsockopt needs it on an
 * already-established pcb, where re-running that function would also reinstall
 * the callbacks and re-apply every other socket option.  Requires
 * g_lwip_lock held: it writes pcb state.
 */

#include "net/socket_inet_opt_state.h"

/* The snapshot Rust reads and writes is the C struct the send paths carry. */
_Static_assert(sizeof(net_inet_opt_snapshot_t) ==
                   sizeof(net_inet_ip_options_snapshot_t) &&
               offsetof(net_inet_opt_snapshot_t, ttl) ==
                   offsetof(net_inet_ip_options_snapshot_t, ttl) &&
               offsetof(net_inet_opt_snapshot_t, tos) ==
                   offsetof(net_inet_ip_options_snapshot_t, tos) &&
               offsetof(net_inet_opt_snapshot_t, mc_ttl) ==
                   offsetof(net_inet_ip_options_snapshot_t, mc_ttl) &&
               offsetof(net_inet_opt_snapshot_t, mc_loop) ==
                   offsetof(net_inet_ip_options_snapshot_t, mc_loop) &&
               offsetof(net_inet_opt_snapshot_t, generation) ==
                   offsetof(net_inet_ip_options_snapshot_t, generation),
               "the Rust snapshot must mirror net_inet_ip_options_snapshot_t");

/*
 * The option and buffer machinery is Rust (kernel/net/backends/lwip/socket_inet_opt.rs).
 * What is left here is the primitive set it calls.
 */

void a20_net_inet_opt_view_init(void *sp, net_inet_opt_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->sock_type = &s->type;
    out->tcp_nodelay = &s->tcp_nodelay;
    out->tcp_congestion = &s->tcp_congestion;
    out->snd_buf = &s->snd_buf;
    out->rcv_buf = &s->rcv_buf;
    out->keepalive = &s->keepalive;
    out->keep_idle = &s->keep_idle;
    out->keep_intvl = &s->keep_intvl;
    out->keep_cnt = &s->keep_cnt;
    out->reuseaddr = &s->reuseaddr;
    out->ip_ttl_set = &s->ip_ttl_set;
    out->ip_ttl = &s->ip_ttl;
    out->ip_tos_set = &s->ip_tos_set;
    out->ip_tos = &s->ip_tos;
    out->mc_ttl = &s->mc_ttl;
    out->mc_loop = &s->mc_loop;
    out->ip_opts_generation = &s->ip_opts_generation;
    out->udp = (void **)&lw(s)->udp;
    out->raw = (void **)&lw(s)->raw;
    out->tcp = (void **)&lw(s)->tcp;
}

void a20_net_inet_opt_sock_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_inet_opt_sock_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

uint64_t a20_net_inet_opt_lane_lock(void *s)
{
    return net_inet_socket_lane_lock((net_socket_t *)s);
}

void a20_net_inet_opt_lane_unlock(uint64_t flags)
{
    a20_lwip_lane_unlock(flags);
}

uint64_t a20_net_inet_opt_shared_lock(void *pcb, int kind)
{
    return a20_lwip_shared_lock(kind == 0 ? A20_LWIP_SHARED_UDP
                                          : A20_LWIP_SHARED_RAW, pcb);
}

void a20_net_inet_opt_shared_unlock(void *pcb, int kind, uint64_t flags)
{
    a20_lwip_shared_unlock(kind == 0 ? A20_LWIP_SHARED_UDP
                                     : A20_LWIP_SHARED_RAW, pcb, flags);
}

uint32_t a20_net_inet_opt_tcp_snd_buf(void)
{
    return (uint32_t)TCP_SND_BUF;
}

uint32_t a20_net_inet_opt_tcp_wnd(void)
{
    return (uint32_t)TCP_WND;
}

uint32_t a20_net_inet_opt_tcp_rcv_scale(void)
{
    return (uint32_t)TCP_RCV_SCALE;
}

uint32_t a20_net_inet_opt_max_payload(void)
{
    return (uint32_t)NET_MAX_PAYLOAD;
}

uint32_t a20_net_inet_opt_dgram_rcv_default(void)
{
    return NET_DGRAM_RCV_BUF_DEFAULT;
}

int a20_net_inet_opt_pcb_is_listen(void *pcb)
{
    return ((struct tcp_pcb *)pcb)->state == LISTEN ? 1 : 0;
}

void a20_net_inet_opt_set_snd_buf(void *pcb, uint32_t v)
{
    ((struct tcp_pcb *)pcb)->snd_buf = (tcpwnd_size_t)v;
}

void a20_net_inet_opt_set_wnd_limit(void *pcb, uint32_t v)
{
#ifndef CONFIG_LWIP_UPSTREAM
    ((struct tcp_pcb *)pcb)->wnd_limit = (tcpwnd_size_t)v;
#else
    (void)pcb; (void)v; /* Smaller native windows are rejected at setsockopt. */
#endif
}

uint32_t a20_net_inet_opt_get_rcv_wnd(void *pcb)
{
    return (uint32_t)((struct tcp_pcb *)pcb)->rcv_wnd;
}

void a20_net_inet_opt_set_rcv_wnd(void *pcb, uint32_t v)
{
    ((struct tcp_pcb *)pcb)->rcv_wnd = (tcpwnd_size_t)v;
}

void a20_net_inet_opt_set_rcv_ann_wnd(void *pcb, uint32_t v)
{
    ((struct tcp_pcb *)pcb)->rcv_ann_wnd = (tcpwnd_size_t)v;
}

int a20_net_inet_opt_pcb_established(void *pcb)
{
    u8_t st = ((struct tcp_pcb *)pcb)->state;
    return st == ESTABLISHED || st == CLOSE_WAIT || st == LAST_ACK ||
           st == FIN_WAIT_1 || st == FIN_WAIT_2 || st == CLOSING ||
           st == TIME_WAIT;
}

uint32_t a20_net_inet_opt_get_rcv_ann_wnd(void *pcb)
{
    return (uint32_t)((struct tcp_pcb *)pcb)->rcv_ann_wnd;
}

void a20_net_inet_opt_recved0(void *pcb)
{
    tcp_recved((struct tcp_pcb *)pcb, 0);
}

void a20_net_inet_opt_nagle_disable(void *pcb)
{
    tcp_nagle_disable((struct tcp_pcb *)pcb);
}

int a20_net_inet_opt_cong_get(void *pcb)
{
#if LWIP_TCP_CUBIC
    return (int)((struct tcp_pcb *)pcb)->cong_alg;
#else
    (void)pcb;
    return 0; /* TCP_CONG_RENO */
#endif
}

void a20_net_inet_opt_cong_set(void *pcb, int alg)
{
#if LWIP_TCP_CUBIC
    ((struct tcp_pcb *)pcb)->cong_alg = (uint8_t)alg;
#else
    (void)pcb;
    (void)alg;
#endif
}

void a20_net_inet_opt_cubic_init(void *pcb)
{
#if LWIP_TCP_CUBIC
    tcp_cubic_init((struct tcp_pcb *)pcb);
#else
    (void)pcb;
#endif
}

void a20_net_inet_opt_set_keepalive(void *pcb)
{
    ((struct tcp_pcb *)pcb)->so_options |= SOF_KEEPALIVE;
}

void a20_net_inet_opt_set_reuseaddr(void *pcb)
{
    ip_set_option((struct tcp_pcb *)pcb, SOF_REUSEADDR);
}

void a20_net_inet_opt_set_keep_idle(void *pcb, uint32_t v)
{
    ((struct tcp_pcb *)pcb)->keep_idle = v;
}

void a20_net_inet_opt_set_keep_intvl(void *pcb, uint32_t v)
{
    ((struct tcp_pcb *)pcb)->keep_intvl = v;
}

void a20_net_inet_opt_set_keep_cnt(void *pcb, uint32_t v)
{
    ((struct tcp_pcb *)pcb)->keep_cnt = v;
}

void a20_net_inet_opt_set_callbacks(void *s, void *pcb)
{
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    tcp_arg(p, s);
    tcp_recv(p, lwip_tcp_recv_cb);
    tcp_err(p, lwip_tcp_err_cb);
    tcp_sent(p, lwip_tcp_sent_cb);
}

void a20_net_inet_opt_apply_udp(void *pcb, uint8_t ttl, uint8_t tos,
                                uint8_t mc_ttl, uint8_t mc_loop)
{
    struct udp_pcb *p = (struct udp_pcb *)pcb;
    p->ttl = ttl;
    p->tos = tos;
    udp_set_multicast_ttl(p, mc_ttl);
    if (mc_loop)
        udp_set_flags(p, UDP_FLAGS_MULTICAST_LOOP);
    else
        udp_clear_flags(p, UDP_FLAGS_MULTICAST_LOOP);
}

void a20_net_inet_opt_apply_raw(void *pcb, uint8_t ttl, uint8_t tos,
                                uint8_t mc_ttl, uint8_t mc_loop)
{
    struct raw_pcb *p = (struct raw_pcb *)pcb;
    p->ttl = ttl;
    p->tos = tos;
    raw_set_multicast_ttl(p, mc_ttl);
    if (mc_loop)
        raw_set_flags(p, RAW_FLAGS_MULTICAST_LOOP);
    else
        raw_clear_flags(p, RAW_FLAGS_MULTICAST_LOOP);
}

void a20_net_inet_opt_apply_tcp(void *pcb, uint8_t ttl, uint8_t tos)
{
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    p->ttl = ttl;
    p->tos = tos;
}

/* The public entry keeps its typed pcb signature. */
void a20_net_cong_apply(struct tcp_pcb *pcb, uint8_t alg)
{
    a20_net_cong_apply_impl(pcb, alg);
}

uint32_t a20_lwip_transport_buffer_ceiling(net_socket_t *s, int is_snd)
{
    return a20_net_socket_buf_ceiling(s, is_snd);
}

uint32_t a20_lwip_transport_buffer_in_force(net_socket_t *s, int is_snd)
{
    return a20_net_socket_buf_in_force(s, is_snd);
}

/*
 * Buffer autotuning.
 *
 * The per-tier initial value (NET_PROFILE_TCP_BUF_INIT) is deliberately below
 * the protocol ceiling so a caller that never sets SO_SNDBUF/SO_RCVBUF has
 * somewhere to grow.  Growth is doubling on observed demand, bounded by the
 * same ceiling getsockopt reports and by window scaling on the receive side:
 *
 *   receive  a read of at least half the current ceiling doubles it, applied to
 *            the live pcb's wnd_limit under the lane lock;
 *   send     a write that runs out of pcb->snd_buf doubles the socket's ceiling
 *            and grants the difference as new queue room.
 *
 * Never shrinks: reclaiming a live window would need an announce and buys
 * nothing here.  An explicit setsockopt clears the direction's flag, so the
 * caller's value is final.
 */
static uint32_t net_buf_grow(uint32_t current, uint32_t ceiling)
{
    if (ceiling == 0 || current >= ceiling)
        return 0;
    uint32_t next = current ? current : 1;
    while (next < ceiling) {
        uint32_t doubled = next * 2;
        if (doubled <= next || doubled >= ceiling) {
            next = ceiling;
            break;
        }
        next = doubled;
    }
    return next > current ? next : 0;
}

/* Called with the lane lock held; the pcb is the live window. */
static uint32_t net_tcp_rcv_autotune(net_socket_t *s, struct tcp_pcb *pcb,
                                     size_t consumed)
{
#ifdef CONFIG_LWIP_UPSTREAM
    (void)s; (void)pcb; (void)consumed;
    return 0;
#else
    if (!s->rcv_autotune || !pcb)
        return 0;
    uint32_t threshold = s->rcv_buf / 2;
    if (consumed < threshold)
        return 0;
    uint32_t next = net_buf_grow(pcb->wnd_limit,
                                 net_socket_buf_ceiling(s, 0));
    if (next) {
        pcb->wnd_limit = (tcpwnd_size_t)next;
        return next;
    }
    return 0;
#endif
}

/* Called with the lane lock held.  Returns the new socket ceiling, or 0. */
uint32_t a20_net_inet_send_tcp_autotune_grow(void *sp, void *tp)
{
    net_socket_t *s = (net_socket_t *)sp;
    struct tcp_pcb *pcb = (struct tcp_pcb *)tp;
    if (!s || !pcb || !s->snd_autotune)
        return 0;
    uint32_t next = net_buf_grow(s->snd_buf, net_socket_buf_ceiling(s, 1));
    if (!next)
        return 0;
    /* pcb->snd_buf is available space; queued bytes are (old - available).
     * Only call this when available is exhausted, so the delta is exactly the
     * new capacity. */
    if (pcb->snd_buf != 0)
        return 0;
    pcb->snd_buf = (tcpwnd_size_t)(next - s->snd_buf);
    return next;
}

/* Called after the lane lock is released: the socket record is protected by
 * the socket lock, and the two locks are never held together. */
void a20_net_inet_send_tcp_autotune_store(void *sp, uint32_t value)
{
    net_socket_t *s = (net_socket_t *)sp;
    if (!s || value == 0)
        return;
    uint64_t flags = net_sock_lock(s);
    if (s->snd_autotune)
        s->snd_buf = value;
    net_sock_unlock(s, flags);
}

/*
 * Apply SO_SNDBUF / SO_RCVBUF to an lwIP pcb.
 *
 * What each one actually does here, stated plainly because neither is Linux's
 * option and both are weaker than the name suggests:
 *
 *   SO_SNDBUF bounds how many bytes this socket may have sitting in its pcb's
 *   send queue at once.  It is enforced in exactly one place -- pcb->snd_buf,
 *   which is the AVAILABLE space lwIP will let tcp_write() fill -- so
 *   net_inet_send_tcp() asks the pcb how much room is left rather than keeping a
 *   second, parallel estimate.  The socket blocks (or reports a short write)
 *   once the ceiling is reached.  It does NOT bound data in flight on the wire:
 *   that is congestion control's job, and bounding it here would fight the
 *   algorithm.  A raised ceiling is written straight back into pcb->snd_buf, so
 *   it takes effect on a connection that is already up; see the body and
 *   DIVERGENCE.md 2.10.
 *
 *   SO_RCVBUF bounds this pcb's receive window, via the pcb's wnd_limit field
 *   (A20OS divergence, DIVERGENCE.md 2.6).  Without that field the ceiling
 *   would survive exactly until the first read(), because tcp_recved() hands
 *   the window straight back to TCP_WND on every read.
 *
 *   NEITHER is tuned.  There is no auto-tuning, no memory-pressure feedback and
 *   no adjustment from observed throughput: the value a caller sets is the
 *   value in force until it changes it.  Linux grows sk_sndbuf and sk_rcvbuf
 *   from tcp_wmem / tcp_rmem based on SO_SNDBUFFORCE and measured behaviour;
 *   nothing here does, and a caller that relies on that growth will not get it.
 *
 * Clamping, and why: the send ceiling cannot exceed TCP_SND_BUF, which is the
 * pcb's real capacity -- asking for more buys nothing, and the caller is told
 * the clamped value by getsockopt rather than being left to believe otherwise.
 * The receive ceiling cannot exceed this build's configured TCP_WND for the
 * same reason, and additionally cannot break window scaling: the wire field is
 * rcv_wnd >> rcv_scale and is 16 bits, so anything above 0xFFFF << TCP_RCV_SCALE
 * would be truncated to a window the caller did not ask for.  Both bounds are
 * the *configured* constants rather than the pcb's current state; see the
 * comment in the body for why using the pcb's state here is a trap.
 *
 * Values are snapshotted under the socket lock before entering lwIP; this
 * helper only consumes that immutable pair while the caller holds the control
 * barrier.  It never reads or writes socket metadata under a core lock.
 */

/* The buffer and option application are Rust; these wrappers keep the
 * names the socket and control paths call. */

void net_inet_tcp_buf_apply(uint32_t snd_buf, uint32_t rcv_buf,
                            struct tcp_pcb *pcb)
{
    a20_net_inet_tcp_buf_apply(snd_buf, rcv_buf, pcb);
}

void net_inet_tcp_apply_options(net_socket_t *s, struct tcp_pcb *pcb)
{
    a20_net_inet_tcp_apply_options(s, pcb);
}

/*
 * Turn a bound AF_INET socket into a real lwIP listening socket.
 *
 * This is what makes an inbound connection possible.  The alternative,
 * net_listen's historical behaviour, keeps the listener entirely in the socket
 * layer: it sets local_tcp and drops the bound pcb, so no LISTEN pcb exists in
 * lwIP and an inbound SYN is answered with RST because nothing is listening on
 * that port.  A listener that only exists in the socket layer is reachable
 * only by another process in the same kernel using the same shortcut.
 *
 * The accept bookkeeping stays in the socket layer either way -- the accept
 * queue, the child net_socket_t and the wakeup are unchanged -- so this adds
 * reachability, not a second accept model.  Must be called with g_lwip_lock
 * held.
 */

#include "net/socket_inet_life_state.h"

/*
 * The INET socket lifecycle is Rust (kernel/net/backends/lwip/socket_inet_life.rs): pcb
 * creation per type with its callback wiring, the listen conversion, and the
 * teardown.  What is left here is the primitive set it calls and the wrappers.
 */

void a20_net_inet_life_view_init(void *sp, net_inet_life_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = (void **)&s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->protocol = &s->protocol;
    out->udp = (void **)&lw(s)->udp;
    out->raw = (void **)&lw(s)->raw;
    out->tcp = (void **)&lw(s)->tcp;
}

void a20_net_inet_life_lock(uint64_t *flags)
{
    *flags = a20_lwip_lock();
}

void a20_net_inet_life_unlock(uint64_t flags)
{
    a20_lwip_unlock(flags);
}

int a20_net_inet_life_ns_initial(void *ns)
{
    return resource_ns_is_initial((const resource_namespace_t *)ns) ? 1 : 0;
}

unsigned a20_net_inet_life_lane_load(void *sp)
{
    return net_socket_lane_load((const net_socket_t *)sp);
}

void a20_net_inet_life_lane_store(void *sp, unsigned lane)
{
    net_socket_lane_store((net_socket_t *)sp, lane);
}

void a20_net_inet_life_lane_enter(unsigned lane)
{
    /* At one lane the macro discards its argument. */
    (void)lane;
    a20_lwip_lane_enter(lane);
}

unsigned a20_net_inet_life_pcb_owner(void *pcb, int kind)
{
    if (kind == 0)
        return NET_PCB_LANE_OWNER_OF_PCB((struct udp_pcb *)pcb);
    return NET_PCB_LANE_OWNER_OF_PCB((struct tcp_pcb *)pcb);
}

void *a20_net_inet_life_udp_new(int v6)
{
    return udp_new_ip_type(v6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
}

void a20_net_inet_life_udp_cb(void *pcb, void *s)
{
    udp_recv((struct udp_pcb *)pcb, lwip_udp_recv_cb, s);
}

void *a20_net_inet_life_raw_new(int v6, int protocol)
{
    return raw_new_ip_type(v6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4,
                           (u8_t)protocol);
}

void a20_net_inet_life_raw_cb(void *pcb, void *s)
{
    raw_recv((struct raw_pcb *)pcb, lwip_raw_recv_cb, s);
}

void *a20_net_inet_life_tcp_new(int v6)
{
    return tcp_new_ip_type(v6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
}

void a20_net_inet_life_tcp_options(void *s, void *pcb)
{
    net_inet_tcp_apply_options((net_socket_t *)s, (struct tcp_pcb *)pcb);
}

void *a20_net_inet_life_tcp_listen(void *pcb, int backlog)
{
    return tcp_listen_with_backlog((struct tcp_pcb *)pcb, (u8_t)backlog);
}

void a20_net_inet_life_tcp_arg(void *pcb, void *s)
{
    tcp_arg((struct tcp_pcb *)pcb, s);
}

void a20_net_inet_life_tcp_accept(void *pcb)
{
    tcp_accept((struct tcp_pcb *)pcb, lwip_tcp_accept_cb);
}

int a20_net_inet_life_pcb_is_listen(void *pcb)
{
    return ((struct tcp_pcb *)pcb)->state == LISTEN ? 1 : 0;
}

int a20_net_inet_life_tcp_close(void *pcb)
{
    return (int)tcp_close((struct tcp_pcb *)pcb);
}

void a20_net_inet_life_tcp_abort(void *pcb)
{
    tcp_abort((struct tcp_pcb *)pcb);
}

void a20_net_inet_life_tcp_unbind(void *pcb)
{
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    tcp_arg(p, NULL);
    tcp_recv(p, NULL);
    tcp_err(p, NULL);
    tcp_sent(p, NULL);
}

void a20_net_inet_life_accept_purge(void *s)
{
    net_inet_accept_stage_purge((net_socket_t *)s);
}

void a20_net_inet_life_udp_remove(void *pcb)
{
    udp_remove((struct udp_pcb *)pcb);
}

void a20_net_inet_life_raw_remove(void *pcb)
{
    raw_remove((struct raw_pcb *)pcb);
}

int a20_net_inet_life_spill_count(void)
{
    return NET_BH_RING_SIZE;
}

void *a20_net_inet_life_spill_take(void *sp, int i)
{
    net_socket_t *s = (net_socket_t *)sp;
    struct pbuf *p = lw(s)->bh_ring.owned[i];
    lw(s)->bh_ring.owned[i] = NULL;
    return p;
}

void a20_net_inet_life_pbuf_free(void *p)
{
    pbuf_free((struct pbuf *)p);
}

int a20_lwip_transport_tcp_listen(net_socket_t *s, int backlog)
{
    return a20_net_inet_tcp_listen(s, backlog);
}

int a20_lwip_transport_socket_init(net_socket_t *s)
{
    if (s->domain != AF_INET && s->domain != AF_INET6) return 0;
    if (!net_inet_state_attach(s)) return -ENOMEM;
    return a20_net_inet_socket_init(s);
}

void a20_lwip_transport_socket_destroy(net_socket_t *s)
{
    if (s->transport_private) a20_net_inet_socket_destroy(s);
}

/*
 * Bind the socket's pcb to the address bind() recorded.  Both families: the
 * ip_addr_t conversion above is dual-stack and lwIP's udp_bind()/raw_bind()/
 * tcp_bind() all take the family from that struct, so there is no reason to
 * refuse v6 here.  The AF_INET6 early return this used to carry is what made
 * an AF_INET6 stream socket unbindable at the pcb level, and therefore
 * unlistenable (net_listen converts a *bound* pcb) and unreachable from
 * off-box.
 */

#include "net/socket_inet_bind_state.h"

/*
 * The INET bind transaction is Rust (kernel/net/backends/lwip/socket_inet_bind.rs).  What is
 * left here is the primitive set it calls and the wrapper that keeps the C name.
 */

void a20_net_inet_bind_view_init(void *sp, net_inet_bind_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = (void **)&s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->udp = (void **)&lw(s)->udp;
    out->raw = (void **)&lw(s)->raw;
    out->tcp = (void **)&lw(s)->tcp;
}

void a20_net_inet_bind_lock(uint64_t *flags)
{
    *flags = a20_lwip_lock();
}

void a20_net_inet_bind_unlock(uint64_t flags)
{
    a20_lwip_unlock(flags);
}

int a20_net_inet_bind_ns_initial(void *ns)
{
    return resource_ns_is_initial((const resource_namespace_t *)ns) ? 1 : 0;
}

int a20_net_inet_bind_sockaddr_to_ip(const void *addr, size_t len, void *ip,
                                     uint16_t *port)
{
    return net_sockaddr_to_lwip_ip(addr, len, (ip_addr_t *)ip, port);
}

unsigned a20_net_inet_bind_lane_load(void *sp)
{
    return net_socket_lane_load((const net_socket_t *)sp);
}

void a20_net_inet_bind_lane_store(void *sp, unsigned lane)
{
    net_socket_lane_store((net_socket_t *)sp, lane);
}

void a20_net_inet_bind_lane_enter(unsigned lane)
{
    /* At one lane the macro discards its argument; at more it uses it. */
    (void)lane;
    a20_lwip_lane_enter(lane);
}

/* The lane macro dereferences the pcb's local address and port, so the
 * pointer has to carry its type: 0 is a udp_pcb, 1 a tcp_pcb. */
unsigned a20_net_inet_bind_pcb_owner(void *pcb, int kind)
{
    if (kind == 0)
        return NET_PCB_LANE_OWNER_OF_PCB((struct udp_pcb *)pcb);
    return NET_PCB_LANE_OWNER_OF_PCB((struct tcp_pcb *)pcb);
}

unsigned a20_net_inet_bind_lane_of_addr(const void *addr, size_t len,
                                        unsigned fallback)
{
    return net_socket_lane_of_addr(addr, len, fallback);
}

int a20_net_inet_bind_udp(void *pcb, const void *ip, uint16_t port)
{
    return (int)udp_bind((struct udp_pcb *)pcb, (const ip_addr_t *)ip, port);
}

int a20_net_inet_bind_raw(void *pcb, const void *ip)
{
    return (int)raw_bind((struct raw_pcb *)pcb, (const ip_addr_t *)ip);
}

int a20_net_inet_bind_tcp(void *pcb, const void *ip, uint16_t port)
{
    return (int)tcp_bind((struct tcp_pcb *)pcb, (const ip_addr_t *)ip, port);
}

int a20_lwip_transport_bind_pcb(net_socket_t *s, const void *addr, size_t addrlen)
{
    return a20_net_inet_bind_pcb(s, addr, addrlen);
}

static int net_namespace_target(net_socket_t *s, const void *addr, size_t len)
{
    if (resource_ns_is_initial(s->net_ns)) return 0;
    if (!addr || len < sizeof(net_sockaddr_in_t)) return -EINVAL;
    int family = *(const uint16_t *)addr;
    if (family == AF_INET) {
        const net_sockaddr_in_t *in = addr;
        if (((const uint8_t *)&in->sin_addr)[0] != 127) return -ENETUNREACH;
    } else if (family == AF_INET6 && len >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *in = addr;
        static const uint8_t loopback[16] = { [15] = 1 };
        if (memcmp(in->sin6_addr, loopback, 16)) return -ENETUNREACH;
    } else return -EAFNOSUPPORT;
    return __atomic_load_n(&s->net_ns->loopback_up, __ATOMIC_ACQUIRE) ? 0 : -ENETUNREACH;
}


#include "net/socket_inet_connect_state.h"

/*
 * The local TCP connect pairing transaction is Rust
 * (kernel/net/backends/lwip/socket_inet_connect.rs).  What is left here is the primitive set
 * it calls; the stack path below it stays C, because it ends in a loop that
 * parks the task until the handshake completes and scheduling is not this
 * module's to own.
 */

void a20_net_inet_connect_view_init(void *sp, net_inet_connect_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = (void **)&s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->protocol = &s->protocol;
    out->bound = &s->bound;
    out->connected = &s->connected;
    out->ever_connected = &s->ever_connected;
    out->local_tcp = &s->local_tcp;
    out->closed = &s->closed;
    out->listening = &s->listening;
    out->accept_count = &s->accept_count;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->peer_addr = s->peer_addr;
    out->peer_len = &s->peer_len;
    out->peer = (void **)&s->peer;
}

void a20_net_inet_connect_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_inet_connect_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

void a20_net_inet_connect_lock2(void *a, void *b, net_inet_connect_pair_t *pair)
{
    net_sock_pair_t p = net_sock_lock2((net_socket_t *)a, (net_socket_t *)b);
    pair->lo = p.lo;
    pair->hi = p.hi;
    pair->flags = p.flags;
}

void a20_net_inet_connect_unlock2(net_inet_connect_pair_t *in)
{
    net_sock_pair_t p;
    p.lo = in->lo;
    p.hi = in->hi;
    p.flags = in->flags;
    net_sock_unlock2(p);
}

int a20_net_inet_connect_is_live(void *s)
{
    return net_socket_is_live((const net_socket_t *)s) ? 1 : 0;
}

void a20_net_inet_connect_bind_ephemeral(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
}

void *a20_net_inet_connect_alloc(void)
{
    net_socket_t *s = net_socket_alloc();
    if (s && !net_inet_state_attach(s)) { net_socket_free(s); return NULL; }
    return s;
}

void a20_net_inet_connect_free(void *s)
{
    net_socket_free((net_socket_t *)s);
}

int a20_net_inet_connect_sockaddr_port(const void *addr, size_t len,
                                       uint16_t *port)
{
    return net_sockaddr_port(addr, len, port);
}

int a20_net_inet_connect_ns_initial(void *ns)
{
    return resource_ns_is_initial((const resource_namespace_t *)ns) ? 1 : 0;
}

int a20_net_inet_connect_tcp_path_fast(void)
{
    return g_a20_tcp_path == A20_TCP_PATH_FAST ? 1 : 0;
}

int a20_net_inet_connect_is_local_target(const void *addr, size_t len)
{
    return net_sockaddr_is_local_target(addr, len) ? 1 : 0;
}

void *a20_net_inet_connect_find_listener(void *s, uint16_t port)
{
    return net_find_stream_listener((net_socket_t *)s, port);
}

void a20_net_inet_connect_ns_put(void *ns)
{
    resource_ns_put((resource_namespace_t *)ns);
}

void *a20_net_inet_connect_ns_get(void *ns)
{
    return resource_ns_get((resource_namespace_t *)ns);
}

int a20_net_inet_connect_register(void *child)
{
    return net_register_socket_locked((net_socket_t *)child);
}

int a20_net_inet_connect_unregister(void *child)
{
    return net_socket_unregister((net_socket_t *)child) ? 1 : 0;
}

int a20_net_inet_connect_queue_push(void *listener, void *child)
{
    return net_accept_queue_push_locked((net_socket_t *)listener,
                                        (net_socket_t *)child);
}

void a20_net_inet_connect_wake_accept(void *listener, void *wake_q)
{
    net_socket_t *l = (net_socket_t *)listener;
    net_event_notify(l, A20_EVENT_ACCEPT_READY, 0, 0);
    (void)wait_queue_collect_one(&l->accept_waitq, 0, PROC_WAKE_EVENT,
                                 (proc_wake_q_t *)wake_q);
}

void a20_net_inet_connect_wakeq_init(void *wake_q)
{
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void a20_net_inet_connect_wakeq_flush(void *wake_q)
{
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

void a20_net_inet_connect_drop_pcb(void *s)
{
    net_tcp_drop_pcb((net_socket_t *)s);
}

void a20_net_inet_connect_trace(int which)
{
    if (which == 0)
        ktrace_net("[NET] connect: pushed child to listener accept queue\n");
    else
        ktrace_net("[NET] connect: local TCP connect done\n");
}

/* Core transport half-close. No socket/bucket lock may be held while lwIP
 * stages FIN/error callbacks; the normal bottom half publishes their events. */
int a20_lwip_transport_shutdown(net_socket_t *s, int receive, int transmit)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6) ||
        s->type != SOCK_STREAM || s->local_tcp)
        return 0;
    if (receive && transmit) {
        /* tcp_shutdown(...,1,1) may free the pcb. The full close path clears
         * callbacks and ownership; never tcp_output a consumed pointer. */
        net_tcp_close_pcb(s);
        return 0;
    }
    uint64_t irq = a20_lwip_lock();
    a20_lwip_lane_enter(net_socket_lane_load(s));
    err_t error = ERR_OK;
    if (lw(s)->tcp) {
        int closing = lw(s)->tcp->state == FIN_WAIT_1 || lw(s)->tcp->state == FIN_WAIT_2 ||
            lw(s)->tcp->state == CLOSING || lw(s)->tcp->state == LAST_ACK || lw(s)->tcp->state == TIME_WAIT;
        if (!(transmit && !receive && closing)) error = tcp_shutdown(lw(s)->tcp, receive, transmit);
    }
    if (error == ERR_OK && lw(s)->tcp) (void)tcp_output(lw(s)->tcp);
    a20_lwip_unlock(irq);
    return error == ERR_OK ? 0 : error == ERR_MEM ? -ENOMEM : -ENOTCONN;
}

static int net_inet_connect_stream(net_socket_t *s, const void *addr, size_t addrlen,
                                   const void *connect_addr, size_t peer_len)
{
    int lr = a20_net_inet_connect_local(s, connect_addr, peer_len, NET_MAX_QUEUE);
    if (lr != 1)
        return lr;

    ip_addr_t ip;
    uint16_t port = 0;
    int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
    if (r < 0)
        return r;
    if (g_a20_tcp_path == A20_TCP_PATH_FAST &&
        net_sockaddr_is_local_target(addr, addrlen)) {
        s->connected = 0;
        return -ECONNREFUSED;
    }
    s->tcp_connecting = 1;
    s->tcp_err = ERR_INPROGRESS;
    uint64_t lwip_flags = a20_lwip_lock();
    if (!lw(s)->tcp) {
        a20_lwip_unlock(lwip_flags);
        s->connected = 0;
        return -ECONNREFUSED;
    }
    a20_lwip_lane_enter(net_socket_lane_load(s));
    err_t e = tcp_connect(lw(s)->tcp, &ip, port, lwip_tcp_connected_cb);
    /* tcp_connect chooses a concrete local address and may rebucket the PCB.
     * Publish that owner before releasing the all-lane barrier: otherwise the
     * data-path lock retries between the old socket and new PCB forever. */
    if (e == ERR_OK)
        net_socket_lane_store(s, NET_PCB_LANE_OWNER_OF_PCB(lw(s)->tcp));
    a20_lwip_unlock(lwip_flags);
    if (e != ERR_OK) {
        s->tcp_connecting = 0;
        return -ENETUNREACH;
    }
    if (s->nonblock)
        return -EINPROGRESS;

    uint64_t timeout = net_socket_timeout_snapshot(s, 1);
    if (!timeout)
        timeout = NET_CONNECT_TIMEOUT_TICKS;
    uint64_t deadline = timer_get_ticks() + timeout;
    for (;;) {
        task_t *cur = proc_current();
        if (!cur) {
            a20_lwip_poll();
            continue;
        }
        /* One socket, so one lock: the handshake completion flag, the
         * signal check and the wait-queue link all belong to s. */
        uint64_t wait_irq = net_sock_lock(s);
        if (!s->tcp_connecting) {
            net_sock_unlock(s, wait_irq);
            break;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_sock_unlock(s, wait_irq);
            net_tcp_drop_pcb(s);
            s->tcp_connecting = 0;
            s->connected = 0;
            return -ERESTARTSYS;
        }
        net_sock_unlock(s, wait_irq);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        wait_irq = net_sock_lock(s);
        if (!s->tcp_connecting) {
            net_sock_unlock(s, wait_irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_sock_unlock(s, wait_irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            net_tcp_drop_pcb(s);
            s->tcp_connecting = 0;
            s->connected = 0;
            return -ERESTARTSYS;
        }
        bool linked = wait_queue_link(&s->read_waitq, &entry, token, 0);
        net_sock_unlock(s, wait_irq);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&s->read_waitq, &entry);
        proc_park_finish(token);
        if (reason == PROC_WAKE_TIMEOUT) {
            net_tcp_drop_pcb(s);
            s->tcp_connecting = 0;
            s->closed = 1;
            return -ETIMEDOUT;
        }
        if (proc_wake_reason_is_task_interrupt(reason) ||
            net_task_has_unblocked_signal(cur)) {
            net_tcp_drop_pcb(s);
            s->tcp_connecting = 0;
            s->connected = 0;
            return -ERESTARTSYS;
        }
    }
    if (s->tcp_err != ERR_OK) {
        s->connected = 0;
        return -ECONNREFUSED;
    }
    return 0;
}

int a20_lwip_transport_connect(net_socket_t *s, const void *addr, size_t addrlen,
                     const void *connect_addr, size_t peer_len)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;
    int ns_result = net_namespace_target(s, connect_addr, peer_len);
    if (ns_result < 0) return ns_result;
    if (!resource_ns_is_initial(s->net_ns) && s->type == SOCK_DGRAM) return 0;
    if (s->type == SOCK_DGRAM) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        if (!lw(s)->udp) {
            a20_lwip_unlock(flags);
            return -ENOTSOCK;
        }
        a20_lwip_lane_enter(net_socket_lane_load(s));
        struct udp_pcb *pcb = lw(s)->udp;
        err_t e = ERR_OK;
        /* Connected UDP pins a concrete local source and lets the protocol
         * reserve an unused port. No loopback placeholder is published. */
        if (ip_addr_isany(&pcb->local_ip)) {
            struct netif *n = pcb->netif_idx != NETIF_NO_INDEX
                ? netif_get_by_index(pcb->netif_idx) : ip_route(&pcb->local_ip, &ip);
            const ip_addr_t *source = n ? ip_netif_get_local_ip(n, &ip) : NULL;
            if (!source || ip_addr_isany(source)) e = ERR_RTE;
            else e = udp_bind(pcb, source, pcb->local_port);
        }
        if (e == ERR_OK) e = udp_connect(pcb, &ip, port);
        uint8_t local[NET_SOCKADDR_MAX]; size_t local_length = 0;
        if (e == ERR_OK) {
            net_socket_lane_store(s, NET_PCB_LANE_OWNER_OF_PCB(pcb));
            net_lwip_ip_to_sockaddr(&pcb->local_ip, pcb->local_port, local, &local_length);
        }
        a20_lwip_unlock(flags);
        if (e != ERR_OK) return e == ERR_USE ? -EADDRINUSE : -ENETUNREACH;
        flags = net_sock_lock(s);
        int result = net_socket_is_live(s) ? 0 : -ENOTSOCK;
        if (result == 0) {
            memcpy(s->local, local, local_length); s->local_len = local_length; s->bound = 1;
        }
        net_sock_unlock(s, flags);
        return result;
    }
    if (s->type == SOCK_RAW && s->domain == AF_INET) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        if (!lw(s)->raw) {
            a20_lwip_unlock(flags);
            return -ENOTSOCK;
        }
        a20_lwip_lane_enter(net_socket_lane_load(s));
        err_t e = raw_connect(lw(s)->raw, &ip);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -ENETUNREACH;
    }
    if (s->type == SOCK_RAW && s->domain == AF_INET6) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        if (!lw(s)->raw) {
            a20_lwip_unlock(flags);
            return -ENOTSOCK;
        }
        a20_lwip_lane_enter(net_socket_lane_load(s));
        err_t e = raw_connect(lw(s)->raw, &ip);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -ENETUNREACH;
    }
    if (s->type == SOCK_STREAM)
        return net_inet_connect_stream(s, addr, addrlen, connect_addr, peer_len);
    return 0;
}

/*
 * Mirror the socket's IPPROTO_IP options into the pcb.  lwIP takes TTL, TOS
 * and the multicast hop count from the pcb at output time
 * (udp.c:udp_sendto_if_src, raw.c:raw_sendto_if, tcp_out.c:tcp_output), not
 * from a global, so the per-socket values have to land there.  An unset TTL
 * still resolves to the Linux default (IPDEFTTL) rather than lwIP's 255, so
 * what getsockopt reports is what goes on the wire.
 *
 * Must be called under the PCB's owning lane; UDP and RAW callers also hold
 * that PCB's shared guard because receive callbacks access these same fields.
 * It must never allocate (see docs/zh/net/network-lock-contract.md).
 */
#define NET_IP_TTL_DEFAULT 64

/* The values a packet from this socket would actually carry.  getsockopt has
 * to report these rather than the stored fields, because "never set" and "set
 * to 0" are different requests and only the effective value is observable on
 * the wire. */

#include "net/socket_inet_opt_state.h"

/* The send paths call this retry loop through its C name. */
static uint64_t net_inet_socket_ip_lane_lock(
    net_socket_t *s, net_inet_ip_options_snapshot_t *opts)
{
    return a20_net_inet_socket_ip_lane_lock(s, opts);
}

void net_inet_ip_options_snapshot(net_socket_t *s,
                                  net_inet_ip_options_snapshot_t *out)
{
    a20_net_inet_ip_options_snapshot(s, out);
}

void a20_lwip_transport_ip_effective(net_socket_t *s, uint8_t *ttl, uint8_t *tos,
                           uint8_t *mc_ttl)
{
    a20_net_inet_ip_effective(s, ttl, tos, mc_ttl);
}

void net_inet_ip_opts_apply_snapshot_locked(
    net_socket_t *s, const net_inet_ip_options_snapshot_t *opts)
{
    a20_net_inet_ip_opts_apply_snapshot_locked(s, opts);
}

void a20_lwip_transport_ip_options_apply(net_socket_t *s)
{
    a20_net_inet_ip_opts_apply(s);
}



#include "net/socket_inet_send_state.h"

/*
 * The UDP and raw send transactions are Rust
 * (kernel/net/backends/lwip/socket_inet_send.rs).  What is left here is the primitive set it
 * calls, and the two wrappers that keep the names the dispatcher uses.
 *
 * Rust owns the datagram ceiling, the lazy bind, the destination resolution and
 * the stale-peer drop, the namespace decision, the local-delivery choice and the
 * whole stack-path ordering.  C owns the locks, the pbuf, the address conversion,
 * the IP-option snapshot and reapply, the lwIP send calls, the enqueue and the
 * namespace tests.
 */

/* The opaque buffers Rust carries: the ip address and the option snapshot. */
_Static_assert(sizeof(ip_addr_t) <= 32,
               "kernel/net/backends/lwip/socket_inet_send.rs sizes its ip buffer for ip_addr_t");
_Static_assert(sizeof(net_inet_ip_options_snapshot_t) <= 32,
               "kernel/net/backends/lwip/socket_inet_send.rs sizes its snapshot buffer");

void a20_net_inet_send_view_init(void *sp, net_inet_send_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = (void **)&s->net_ns;
    out->domain = &s->domain;
    out->bound = &s->bound;
    out->connected = &s->connected;
    out->nonblock = &s->nonblock;
    out->send_timeout_ticks = &s->send_timeout_ticks;
    out->rx_count = &s->rx_count;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->peer_addr = s->peer_addr;
    out->peer_len = &s->peer_len;
    out->peer = (void **)&s->peer;
    out->udp = (void **)&lw(s)->udp;
    out->raw = (void **)&lw(s)->raw;
}

void a20_net_inet_send_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_inet_send_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

void a20_net_inet_send_lock2(void *a, void *b, net_inet_send_pair_t *pair)
{
    net_sock_pair_t p = net_sock_lock2((net_socket_t *)a, (net_socket_t *)b);
    pair->lo = p.lo;
    pair->hi = p.hi;
    pair->flags = p.flags;
}

void a20_net_inet_send_unlock2(net_inet_send_pair_t *in)
{
    net_sock_pair_t p;
    p.lo = in->lo;
    p.hi = in->hi;
    p.flags = in->flags;
    net_sock_unlock2(p);
}

int a20_net_inet_send_is_live(void *s)
{
    return net_socket_is_live((const net_socket_t *)s) ? 1 : 0;
}

void a20_net_inet_send_ref(void *s)
{
    net_socket_ref((net_socket_t *)s);
}

void a20_net_inet_send_free(void *s)
{
    net_socket_free((net_socket_t *)s);
}

uint32_t a20_net_inet_send_buf_in_force(void *s)
{
    return net_socket_buf_in_force((net_socket_t *)s, 1);
}

uint16_t a20_net_inet_send_ephemeral_port(void)
{
    return net_alloc_ephemeral_port_locked();
}

static int net_udp_autobind(net_socket_t *s, uint16_t port, int v6) {
    if (!resource_ns_is_initial(s->net_ns)) {
        uint64_t flags = net_sock_lock(s);
        if (!s->bound) net_sockaddr_loopback(s, port);
        net_sock_unlock(s, flags);
        return 0;
    }
    uint8_t address[NET_SOCKADDR_MAX]; size_t length = 0;
    uint64_t flags = a20_lwip_lock();
    struct udp_pcb *pcb = lw(s) ? lw(s)->udp : NULL;
    if (!pcb) { a20_lwip_unlock(flags); return -ENOTSOCK; }
    a20_lwip_lane_enter(net_socket_lane_load(s));
    ip_addr_t any;
    if (v6) ip_addr_set_zero_ip6(&any); else ip_addr_set_zero_ip4(&any);
    /* A concurrent first send/bind may already have selected a port. Use that
     * value; never publish a second frontend port over the same live PCB. */
    /* Port zero lets the protocol owner probe/reserve atomically. A blind
     * frontend counter can pick a port already held by an explicit bind. */
    err_t error = pcb->local_port ? ERR_OK : udp_bind(pcb, &any, 0);
    if (error == ERR_OK) {
        net_socket_lane_store(s, NET_PCB_LANE_OWNER_OF_PCB(pcb));
        net_lwip_ip_to_sockaddr(&pcb->local_ip, pcb->local_port, address, &length);
    }
    a20_lwip_unlock(flags);
    if (error != ERR_OK) return error == ERR_USE ? -EADDRINUSE : -EIO;
    flags = net_sock_lock(s);
    int result = net_socket_is_live(s) ? 0 : -ENOTSOCK;
    if (result == 0 && !s->bound) {
        memcpy(s->local, address, length); s->local_len = length; s->bound = 1;
    }
    net_sock_unlock(s, flags);
    return result;
}
int a20_net_inet_send_bind_v6(void *s, uint16_t port) {
    return net_udp_autobind(s, port, 1);
}
int a20_net_inet_send_bind_v4(void *s, uint16_t port) {
    return net_udp_autobind(s, port, 0);
}

void *a20_net_inet_send_find_udp_dst(void *s, const void *addr, size_t len)
{
    return net_find_udp_dst((net_socket_t *)s, addr, len);
}

int a20_net_inet_send_namespace_target(void *s, const void *addr, size_t len)
{
    return net_namespace_target((net_socket_t *)s, addr, len);
}

int a20_net_inet_send_ns_initial(void *s)
{
    return resource_ns_is_initial(((const net_socket_t *)s)->net_ns) ? 1 : 0;
}

int a20_net_inet_send_enqueue_locked(void *dst, const void *buf, size_t len,
                                     const uint8_t *src_local, size_t src_len)
{
    return net_enqueue_msg_locked((net_socket_t *)dst, buf, len, src_local,
                                  src_len);
}

int a20_net_inet_send_enqueue_blocking(void *s, void *dst, const void *buf,
                                       size_t len, const uint8_t *src_local,
                                       size_t src_len, int dontwait,
                                       uint64_t timeout)
{
    return net_enqueue_msg_blocking((net_socket_t *)s, (net_socket_t *)dst, buf,
                                    len, src_local, src_len, dontwait, timeout);
}

void a20_net_inet_send_collect_read(void *dst, void *wake_q)
{
    (void)wait_queue_collect_one(&((net_socket_t *)dst)->read_waitq, 0,
                                 PROC_WAKE_EVENT, (proc_wake_q_t *)wake_q);
}

void a20_net_inet_send_wakeq_init(void *wake_q)
{
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void a20_net_inet_send_wakeq_flush(void *wake_q)
{
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

void *a20_net_inet_send_pbuf_alloc(size_t len)
{
    return pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
}

void a20_net_inet_send_pbuf_take(void *p, const void *buf, size_t len)
{
    (void)pbuf_take((struct pbuf *)p, buf, (u16_t)len);
}

void a20_net_inet_send_pbuf_free(void *p)
{
    pbuf_free((struct pbuf *)p);
}

int a20_net_inet_send_sockaddr_to_ip(const void *addr, size_t len, void *ip,
                                     uint16_t *port)
{
    return net_sockaddr_to_lwip_ip(addr, len, (ip_addr_t *)ip, port);
}

uint64_t a20_net_inet_send_ip_lane_lock(void *s, void *opts)
{
    return net_inet_socket_ip_lane_lock(
        (net_socket_t *)s, (net_inet_ip_options_snapshot_t *)opts);
}

void a20_net_inet_send_lane_unlock(uint64_t flags)
{
    a20_lwip_lane_unlock(flags);
}

void a20_net_inet_send_ip_opts_apply(void *s, const void *opts)
{
    net_inet_ip_opts_apply_snapshot_locked(
        (net_socket_t *)s, (const net_inet_ip_options_snapshot_t *)opts);
}

int a20_net_inet_send_udp_sendto(void *pcb, void *p, const void *ip, uint16_t port)
{
    return (int)udp_sendto((struct udp_pcb *)pcb, (struct pbuf *)p,
                           (const ip_addr_t *)ip, port);
}

int a20_net_inet_send_udp_send(void *pcb, void *p)
{
    return (int)udp_send((struct udp_pcb *)pcb, (struct pbuf *)p);
}

/* Per-call metadata never changes a shared PCB. Lane/config exclusion keeps
 * netif identities stable for the duration of this synchronous library call. */
int a20_net_inet_send_udp_control_packet(void *upcb, void *p, const void *dest,
    uint16_t port, uint32_t ifindex, uint32_t source)
{
    struct udp_pcb *pcb = upcb;
    const ip_addr_t *target = dest;
    if (!target) {
        if (!(pcb->flags & UDP_FLAGS_CONNECTED)) return -EDESTADDRREQ;
        target = &pcb->remote_ip;
        port = pcb->remote_port;
    }
    if (!IP_IS_V4(target)) return -EAFNOSUPPORT;
    if (ifindex > 255) return -ENODEV;
    if (pcb->netif_idx != NETIF_NO_INDEX) {
        if (ifindex && ifindex != pcb->netif_idx) return -EINVAL;
        ifindex = pcb->netif_idx;
    }
    ip4_addr_t src4 = {.addr = source};
    struct netif *n = ifindex ? netif_get_by_index((u8_t)ifindex) :
        ip4_route_src(source ? &src4 : NULL, ip_2_ip4(target));
    if (!n) return ifindex ? -ENODEV : -ENETUNREACH;
    if (source && !ip4_addr_cmp(&src4, netif_ip4_addr(n))) return -EADDRNOTAVAIL;
    if (!source) {
        if (IP_IS_V4(&pcb->local_ip) && !ip4_addr_isany(ip_2_ip4(&pcb->local_ip)))
            src4 = *ip_2_ip4(&pcb->local_ip);
        else src4 = *netif_ip4_addr(n);
    }
    ip_addr_t src;
    ip_addr_copy_from_ip4(src, src4);
    err_t e = udp_sendto_if_src(pcb, p, target, port, n, &src);
    return e == ERR_OK ? 0 : e == ERR_MEM ? -ENOMEM :
        e == ERR_RTE ? -ENETUNREACH : -EIO;
}

int a20_net_inet_send_udp_connected(void *pcb)
{
    return (((struct udp_pcb *)pcb)->flags & UDP_FLAGS_CONNECTED) ? 1 : 0;
}

int a20_net_inet_send_raw_sendto(void *pcb, void *p, const void *ip)
{
    return (int)raw_sendto((struct raw_pcb *)pcb, (struct pbuf *)p,
                           (const ip_addr_t *)ip);
}

int a20_net_inet_send_raw_send(void *pcb, void *p)
{
    return (int)raw_send((struct raw_pcb *)pcb, (struct pbuf *)p);
}

int a20_net_inet_send_raw_connected(void *pcb)
{
    return (((struct raw_pcb *)pcb)->flags & RAW_FLAGS_CONNECTED) ? 1 : 0;
}

uint64_t a20_net_inet_send_shared_lock(int kind, void *pcb)
{
    return a20_lwip_shared_lock(kind == A20_NET_SEND_SHARED_UDP
                                    ? A20_LWIP_SHARED_UDP
                                    : A20_LWIP_SHARED_RAW, pcb);
}

void a20_net_inet_send_shared_unlock(int kind, void *pcb, uint64_t flags)
{
    a20_lwip_shared_unlock(kind == A20_NET_SEND_SHARED_UDP
                               ? A20_LWIP_SHARED_UDP
                               : A20_LWIP_SHARED_RAW, pcb, flags);
}

void a20_net_inet_send_poll_locked(void)
{
    a20_lwip_poll_locked();
}

static int net_inet_send_udp(net_socket_t *s, const void *buf, size_t len,
                             int flags, const void *addr, size_t addrlen)
{
    return a20_net_inet_send_udp(s, buf, len, flags, addr, addrlen,
                                 NET_MAX_QUEUE);
}

int a20_lwip_transport_send_udp_control(net_socket_t *s, const void *buf, size_t len,
    int flags, const void *addr, size_t addrlen,
    const net_datagram_control_t *control)
{
    return a20_net_inet_send_udp_control(s, buf, len, flags, addr, addrlen,
                                        NET_MAX_QUEUE, control);
}

static int net_inet_send_raw(net_socket_t *s, const void *buf, size_t len,
                             const void *addr, size_t addrlen)
{
    return a20_net_inet_send_raw(s, buf, len, addr, addrlen, NET_MAX_QUEUE);
}


#include "net/socket_inet_send_tcp_state.h"

/*
 * The TCP stream send transaction is Rust
 * (kernel/net/backends/lwip/socket_inet_send_tcp.rs): the early refusals, the local
 * socket-to-socket path, and the stack-path retry loop.  What is left here is
 * the primitive set it calls -- including the blocking wait, which is the
 * scheduler's protocol and stays in one piece -- and the wrapper.
 */

void a20_net_inet_send_tcp_view_init(void *sp, net_inet_send_tcp_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->connected = &s->connected;
    out->closed = &s->closed;
    out->ever_connected = &s->ever_connected;
    out->shut_wr = &s->shut_wr;
    out->local_tcp = &s->local_tcp;
    out->nonblock = &s->nonblock;
    out->send_timeout_ticks = &s->send_timeout_ticks;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->peer = (void **)&s->peer;
    out->tcp = (void **)&lw(s)->tcp;
}

void a20_net_inet_send_tcp_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_inet_send_tcp_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

void a20_net_inet_send_tcp_lock2(void *a, void *b,
                                 net_inet_send_tcp_pair_t *pair)
{
    net_sock_pair_t p = net_sock_lock2((net_socket_t *)a, (net_socket_t *)b);
    pair->lo = p.lo;
    pair->hi = p.hi;
    pair->flags = p.flags;
}

void a20_net_inet_send_tcp_unlock2(net_inet_send_tcp_pair_t *in)
{
    net_sock_pair_t p;
    p.lo = in->lo;
    p.hi = in->hi;
    p.flags = in->flags;
    net_sock_unlock2(p);
}

int a20_net_inet_send_tcp_is_live(void *s)
{
    return net_socket_is_live((const net_socket_t *)s) ? 1 : 0;
}

void a20_net_inet_send_tcp_ref(void *s)
{
    net_socket_ref((net_socket_t *)s);
}

void a20_net_inet_send_tcp_free(void *s)
{
    net_socket_free((net_socket_t *)s);
}

int a20_net_inet_send_tcp_notconn(int why)
{
    return net_notconn((net_notconn_reason_t)why);
}

int a20_net_inet_send_tcp_enqueue_blocking(void *s, void *dst, const void *buf,
                                           size_t len, const uint8_t *src_local,
                                           size_t src_len, int nonblock,
                                           uint64_t timeout)
{
    return net_enqueue_msg_blocking((net_socket_t *)s, (net_socket_t *)dst, buf,
                                    len, src_local, src_len, nonblock, timeout);
}

uint64_t a20_net_inet_send_tcp_now(void)
{
    return timer_get_ticks();
}

uint64_t a20_net_inet_send_tcp_ip_lane_lock(void *s, void *opts)
{
    return net_inet_socket_ip_lane_lock(
        (net_socket_t *)s, (net_inet_ip_options_snapshot_t *)opts);
}

void a20_net_inet_send_tcp_lane_unlock(uint64_t flags)
{
    a20_lwip_lane_unlock(flags);
}

void a20_net_inet_send_tcp_ip_opts_apply(void *s, const void *opts)
{
    net_inet_ip_opts_apply_snapshot_locked(
        (net_socket_t *)s, (const net_inet_ip_options_snapshot_t *)opts);
}

void a20_net_inet_send_tcp_poll_locked(void)
{
    a20_lwip_poll_locked();
}

uint32_t a20_net_inet_send_tcp_snd_buf_max(void)
{
    return (uint32_t)TCP_SND_BUF;
}

uint32_t a20_net_inet_send_tcp_snd_queuelen_max(void)
{
    return (uint32_t)TCP_SND_QUEUELEN;
}

uint32_t a20_net_inet_send_tcp_pcb_snd_buf(void *tcp)
{
    return (uint32_t)((struct tcp_pcb *)tcp)->snd_buf;
}

uint32_t a20_net_inet_send_tcp_pcb_snd_queuelen(void *tcp)
{
    return (uint32_t)((struct tcp_pcb *)tcp)->snd_queuelen;
}

int a20_net_inet_send_tcp_write(void *tcp, const uint8_t *buf, size_t len)
{
    return (int)tcp_write((struct tcp_pcb *)tcp, buf, (u16_t)len,
                          TCP_WRITE_FLAG_COPY);
}

int a20_net_inet_send_tcp_output(void *tcp)
{
    return (int)tcp_output((struct tcp_pcb *)tcp);
}

void a20_net_inet_send_tcp_trace_deferred(int err)
{
    /* The trace macro compiles away when tracing is off. */
    (void)err;
    ktrace_net("[NET] queued TCP output deferred: %d\n", err);
}

/*
 * The blocking wait for room: link the waiter, re-check the window with the lane
 * lock, try to wake ourselves if an ACK already landed, then park.  Kept whole
 * because it is the scheduler's protocol; returns 0 to retry the loop.
 */
int a20_net_inet_send_tcp_wait(void *sp, uint64_t start, int allocation_blocked)
{
    net_socket_t *s = (net_socket_t *)sp;
    task_t *cur = proc_current();
    if (!cur)
        return -EAGAIN;
    if (net_task_has_unblocked_signal(cur))
        return -ERESTARTSYS;
    if (net_socket_wait_expired(s, start, 1))
        return -EAGAIN;
    uint64_t timeout = net_socket_timeout_snapshot(s, 1);
    uint64_t deadline = timeout ? start + timeout : 0;
    /* Allocation can fail even with an empty queue.  Retry after a tick instead
     * of waiting forever for an ACK that cannot arrive. */
    if (allocation_blocked) {
        uint64_t retry = timer_get_ticks() + 1;
        if (!deadline || retry < deadline)
            deadline = retry;
    }
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
    if (!token.task)
        return -EAGAIN;

    wait_queue_entry_t entry = {0};
    uint64_t irq = net_sock_lock(s);
    bool linked = wait_queue_link(&s->write_waitq, &entry, token, 0);
    net_sock_unlock(s, irq);
    uint64_t room_flags = net_inet_socket_lane_lock(s);
    int room_now = 0;
    if (lw(s)->tcp && !s->closed && s->connected) {
        /* Has an ACK landed since we parked, and does that mean room? */
        room_now = !allocation_blocked && lw(s)->tcp->snd_buf > 0 &&
                   lw(s)->tcp->snd_queuelen < TCP_SND_QUEUELEN;
    }
    a20_lwip_lane_unlock(room_flags);
    if (room_now)
        (void)proc_try_wake(cur, token.seq, PROC_WAKE_EVENT);
    proc_wake_reason_t reason;
    if (linked)
        reason = proc_park_commit(token);
    else {
        (void)proc_park_cancel(token);
        reason = PROC_WAKE_CANCEL;
    }
    wait_queue_unlink(&s->write_waitq, &entry);
    proc_park_finish(token);
    if (proc_wake_reason_is_task_interrupt(reason))
        return -ERESTARTSYS;
    if (reason == PROC_WAKE_TIMEOUT &&
        (!allocation_blocked || net_socket_wait_expired(s, start, 1)))
        return -EAGAIN;
    return 0;
}

void a20_net_inet_send_tcp_bottom_half_all(void)
{
    net_inet_bottom_half_process_all();
}

void a20_net_inet_send_tcp_packet_bottom_half(void)
{
    net_packet_bottom_half_process();
}

static int net_inet_send_tcp(net_socket_t *s, const void *buf, size_t len)
{
    return a20_net_inet_send_tcp(s, buf, len);
}

/* Interface binding lives in the native PCB. Accepted TCP PCBs inherit this
 * index in tcp_in; keeping a second name cache on the frontend would diverge
 * after accept or a concurrent setter. The cold barrier serializes setters,
 * close, interface removal and all protocol users. */
int a20_lwip_transport_bind_device(net_socket_t *s, const char *name)
{
    uint64_t flags = a20_lwip_lock();
    struct netif *n = NULL;
    if (name[0]) {
        n = netif_find(name);
        if (!n) n = netif_get_by_index((u8_t)net_tun_interface_by_name(name));
        if (!n) { a20_lwip_unlock(flags); return -ENODEV; }
    }
    u8_t index = n ? netif_get_index(n) : NETIF_NO_INDEX;
    int result = 0;
    if (lw(s)->udp) lw(s)->udp->netif_idx = index;
    else if (lw(s)->raw) lw(s)->raw->netif_idx = index;
    else if (lw(s)->tcp) lw(s)->tcp->netif_idx = index;
    else result = -EOPNOTSUPP;
    a20_lwip_unlock(flags);
    return result;
}

int a20_lwip_transport_bound_device(net_socket_t *s, char name[16])
{
    if (!lw(s)) return -EOPNOTSUPP;
    memset(name, 0, 16);
    uint64_t flags = a20_lwip_lock();
    u8_t index = lw(s)->udp ? lw(s)->udp->netif_idx : lw(s)->raw ? lw(s)->raw->netif_idx :
        lw(s)->tcp ? lw(s)->tcp->netif_idx : NETIF_NO_INDEX;
    struct netif *n = index ? netif_get_by_index(index) : NULL;
    if (n && !net_tun_interface_name(netif_get_index(n), name, 16))
        snprintf(name, 16, "%c%c%u", n->name[0], n->name[1], n->num);
    a20_lwip_unlock(flags);
    return index && !n ? -ENODEV : 0;
}

/* lwIP's current output permits IPv4 fragmentation (DF clear). Expose that
 * actual mode; WANT/DO/PROBE require PMTU/DF plumbing and must not fake success. */
int a20_lwip_transport_pmtu_discover(net_socket_t *s, int mode, int set)
{
    if (!s || s->domain != AF_INET) return -EOPNOTSUPP;
#if IP_FRAG
    return !set || mode == 0 ? 0 : -EOPNOTSUPP;
#else
    (void)mode; (void)set;
    return -EOPNOTSUPP;
#endif
}

int a20_lwip_transport_sendto(net_socket_t *s, const void *buf, size_t len,
                    int flags, const void *addr, size_t addrlen)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return -EAFNOSUPPORT;
    if (s->type == SOCK_DGRAM)
        return net_inet_send_udp(s, buf, len, flags, addr, addrlen);
    if (s->type == SOCK_RAW)
        return net_inet_send_raw(s, buf, len, addr, addrlen);
    if (s->type == SOCK_STREAM)
        return net_inet_send_tcp(s, buf, len);
    return -EOPNOTSUPP;
}

int a20_lwip_transport_send_raw_packet(net_socket_t *s, const void *buf, size_t len,
                              const void *addr, size_t addrlen, int hops, int tclass) {
    if (!s || s->domain != AF_INET6 || s->type != SOCK_RAW || !lw(s))
        return -EOPNOTSUPP;
    if (len > NET_MAX_PAYLOAD) return -EMSGSIZE;
    ip_addr_t target;
    if (addr && net_sockaddr_to_lwip_ip(addr, addrlen, &target, NULL) < 0)
        return -EINVAL;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) return -ENOMEM;
    if (len) pbuf_take(p, buf, (u16_t)len);
    net_inet_ip_options_snapshot_t options;
    uint64_t flags = net_inet_socket_ip_lane_lock(s, &options);
    int result = -ENOTSOCK;
    if (lw(s)->raw) {
        net_inet_ip_opts_apply_snapshot_locked(s, &options);
        /* A synchronous raw send reads this value copy. It is never linked
         * into a PCB list and no shared PCB option is temporarily overwritten. */
        struct raw_pcb packet = *lw(s)->raw;
        if (hops >= 0) packet.ttl = (u8_t)hops;
        if (tclass >= 0) packet.tos = (u8_t)tclass;
        err_t e = addr ? raw_sendto(&packet, p, &target) : raw_send(&packet, p);
        result = e == ERR_OK ? (int)len : e == ERR_MEM ? -ENOMEM : -ENETUNREACH;
    }
    pbuf_free(p);
    a20_lwip_lane_unlock(flags);
    return result;
}

void a20_lwip_transport_accept_child_ready(net_socket_t *s)
{
    if (s && lw(s)) {
        uint64_t flags = net_inet_socket_lane_lock(s);
        /* tcp_backlog_accepted() can emit the handshake ACK, and an ACK that
         * does not fit in the send window is queued, which is a PBUF_POOL
         * allocation. */
        if (lw(s)->tcp)
            tcp_backlog_accepted(lw(s)->tcp);
        a20_lwip_lane_unlock(flags);
    }
}

void a20_lwip_transport_recved(net_socket_t *s, size_t len) {
    if (s && lw(s) && len > 0) {
        size_t consumed = len;
        uint32_t grown = 0;
        uint64_t flags = net_inet_socket_lane_lock(s);
        while (lw(s)->tcp && len > 0) {
            uint16_t n = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
            tcp_recved(lw(s)->tcp, n);
            len -= n;
        }
        if (lw(s)->tcp)
            grown = net_tcp_rcv_autotune(s, lw(s)->tcp, consumed);
        a20_lwip_lane_unlock(flags);
        if (grown) {
            uint64_t sf = net_sock_lock(s);
            if (s->rcv_autotune)
                s->rcv_buf = grown;
            net_sock_unlock(s, sf);
        }
    }
}
