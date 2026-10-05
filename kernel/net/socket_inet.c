#include "net/socket_internal.h"
#include "net/lwip_stack.h"
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
#include "lwip/priv/pcb_lane.h"

/*
 * Next ephemeral port to hand out.
 *
 * This needed no synchronisation of its own while every caller held
 * g_net_lock.  Callers now hold the socket's own lock, and
 * two CPUs binding different sockets hold *different* buckets, so the counter
 * is a compare-exchange.  It stays a global counter: it is not per-socket
 * state, so it does not belong in net_socket_t.
 */
static volatile uint16_t g_next_ephemeral = 49152;

static uint16_t net_htons(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

uint16_t net_ntohs(uint16_t x)
{
    return net_htons(x);
}

uint16_t net_alloc_ephemeral_port_locked(void)
{
    uint16_t p;
    for (;;) {
        uint16_t cur = __atomic_load_n(&g_next_ephemeral, __ATOMIC_RELAXED);
        /* Same floor and same 16-bit wrap as the plain ++ it replaced: nothing
         * below 49152 is ever handed out. */
        uint16_t next = (uint16_t)(cur + 1);
        if (next < 49152)
            next = 49152;
        if (__atomic_compare_exchange_n(&g_next_ephemeral, &cur, next, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            p = cur;
            break;
        }
    }
    return net_htons(p);
}

/*
 * Implicit bind of an unbound inet socket to the loopback address of its own
 * family.  An AF_INET6 socket used to get a 16-byte sockaddr_in with
 * sin_family = AF_INET6 and the v4 loopback in sin_addr, which is not a valid
 * sockaddr_in6 -- the 16 bytes then read back as sin6_flowinfo/sin6_scope_id.
 * getpeername()/getsockname() and the netlink diag path both read that struct
 * back, so a v6 socket's implicit bind reported a nonsense scope id.
 */
void net_sockaddr_loopback(net_socket_t *s, uint16_t port)
{
    if (s->domain == AF_INET6) {
        net_sockaddr_in6_t in6;
        memset(&in6, 0, sizeof(in6));
        in6.sin6_family = AF_INET6;
        in6.sin6_port = port;
        in6.sin6_addr[15] = 1;      /* ::1 */
        memcpy(s->local, &in6, sizeof(in6));
        s->local_len = sizeof(in6);
        s->bound = 1;
        return;
    }
    net_sockaddr_in_t in;
    memset(&in, 0, sizeof(in));
    in.sin_family = (uint16_t)s->domain;
    in.sin_port = port;
    in.sin_addr = 0x0100007fU;
    memcpy(s->local, &in, sizeof(in));
    s->local_len = sizeof(in);
    s->bound = 1;
}

int net_sockaddr_port(const void *addr, size_t len, uint16_t *port)
{
    if (!addr || len < sizeof(net_sockaddr_in_t) || !port)
        return -EINVAL;
    int family = *(const uint16_t *)addr;
    if (family != AF_INET && family != AF_INET6)
        return -EAFNOSUPPORT;
    *port = ((const net_sockaddr_in_t *)addr)->sin_port;
    return 0;
}

void net_sockaddr_set_port(void *addr, size_t len, uint16_t port)
{
    if (!addr || len < sizeof(net_sockaddr_in_t))
        return;
    net_sockaddr_in_t *in = (net_sockaddr_in_t *)addr;
    if (in->sin_family == AF_INET || in->sin_family == AF_INET6)
        in->sin_port = port;
}

int net_sockaddr_in_local(const net_sockaddr_in_t *in)
{
    if (!in)
        return 0;
    uint32_t addr = in->sin_addr;
    if (addr == 0 || addr == 0x0100007fU || addr == 0x0f02000aU)
        return 1;
    return 0;
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
        ip6_addr_t a6;
        memcpy(a6.addr, in6->sin6_addr, sizeof(a6.addr));
        a6.zone = 0;
        ip_addr_copy_from_ip6(*ip, a6);
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
            return net_lane_of(in->sin_addr, in->sin_port);
    }
    if (len >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        if (in6->sin6_family == AF_INET6) {
            uint32_t low;
            memcpy(&low, in6->sin6_addr + 12, sizeof(low));
            return net_lane_of(low, in6->sin6_port);
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

static uint32_t bh_ring_mask(uint32_t idx)
{
    return idx & (NET_BH_RING_SIZE - 1);
}

/*
 * Hand out the next slot, or return -1 when the ring is full.  A slot only
 * comes back into circulation once the consumer has advanced `tail` past it,
 * so any spill reference recorded against it is dead by this point and is
 * released here -- the only place a staged pbuf is freed.  Doing it in the
 * producer keeps memp inside g_lwip_lock, which is the only context where it
 * is currently safe to touch (memp has no internal locking).
 *
 * The slot index is returned so the caller can record a spill reference
 * against it; the event pointer alone would not identify the slot.
 */
static int bh_ring_prepare(net_bh_ring_t *r, net_bh_event_t **out)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if ((head - tail) >= NET_BH_RING_SIZE)
        return -1;
    uint32_t idx = bh_ring_mask(head);
    if (r->owned[idx]) {
        pbuf_free(r->owned[idx]);
        r->owned[idx] = NULL;
    }
    net_bh_event_t *e = &r->events[idx];
    /* Header only.  The inline payload is about to be overwritten up to
     * e->len and is never read past that, so clearing it here would only add
     * back the per-packet memset this split exists to remove. */
    memset(e, 0, __builtin_offsetof(net_bh_event_t, data));
    e->type = NET_BH_RECV;
    *out = e;
    return (int)idx;
}

static void bh_ring_commit(net_bh_ring_t *r)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_fetch_add(&r->head, 1, __ATOMIC_RELAXED);
}

/*
 * Whether `n` more events fit.  The producer is the only writer of head, so a
 * reservation decided here holds until the events are committed.
 */
static bool bh_ring_reserve(net_bh_ring_t *r, uint32_t n)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return ((head - tail) + n) <= NET_BH_RING_SIZE;
}

static net_bh_event_t *bh_ring_consume(net_bh_ring_t *r)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    if (head == tail)
        return NULL;
    a20_perf_count(A20_PERF_NET_BH_EVENTS);
    return &r->events[bh_ring_mask(tail)];
}

static void bh_ring_consume_commit(net_bh_ring_t *r)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __atomic_fetch_add(&r->tail, 1, __ATOMIC_RELEASE);
}

/*
 * Stage `len` bytes of `p` starting at `off` into the event `e` occupies in
 * slot `slot`.  Anything that fits inline is copied; anything larger is held by
 * reference, so an oversized datagram costs a refcount bump rather than a
 * NET_MAX_PAYLOAD-sized staging buffer.
 *
 * A TCP segment always takes the copy arm: TCP_MSS cannot exceed the inline
 * size, asserted below.  Only a datagram socket can reach the spill arm.
 */
static void bh_stage_payload(net_bh_ring_t *r, int slot, net_bh_event_t *e,
                             struct pbuf *p, uint32_t off, size_t len)
{
    if (len <= NET_BH_INLINE_PAYLOAD) {
        pbuf_copy_partial(p, e->data, (u16_t)len, (u16_t)off);
        e->spill = NULL;
        e->spill_off = 0;
        e->len = len;
        return;
    }
    pbuf_ref(p);
    r->owned[slot] = p;
    e->spill = p;
    e->spill_off = off;
    e->len = len;
}

/*
 * lwIP carves the Ethernet, IP and TCP headers out of the head pbuf's payload
 * area, so a segment that exactly fills a PBUF_POOL element has no room for
 * them; and a segment larger than the inline staging buffer would push every
 * TCP receive onto the spill path for no benefit.
 */
_Static_assert(NET_BH_INLINE_PAYLOAD >= TCP_MSS,
               "inline bottom-half staging must cover a full TCP segment");
_Static_assert(NET_BH_INLINE_PAYLOAD < NET_MAX_PAYLOAD,
               "inline staging only makes sense as an optimisation");

/*
 * Stage a whole received TCP segment into s's bottom-half ring, splitting it
 * across as many events as that takes.
 *
 * All or nothing, deliberately.  An lwIP recv callback that returns anything
 * other than ERR_OK is telling lwIP it did *not* take the pbuf: lwIP parks it
 * in pcb->refused_data and hands it back later.  Staging part of a segment and
 * then returning ERR_MEM would therefore replay the whole segment on the retry
 * and duplicate the part already staged, so the capacity for every event this
 * needs is reserved up front and the segment is taken whole or not at all.
 */
static bool net_inet_tcp_stage_payload(net_socket_t *s, struct pbuf *p)
{
    uint32_t need = (uint32_t)((p->tot_len + NET_BH_INLINE_PAYLOAD - 1) /
                               NET_BH_INLINE_PAYLOAD);
    if (need == 0)
        need = 1;
    if (!bh_ring_reserve(&s->bh_ring, need))
        return false;
    size_t off = 0;
    while (off < p->tot_len) {
        net_bh_event_t *e;
        int slot = bh_ring_prepare(&s->bh_ring, &e);
        if (slot < 0)
            return false;
        size_t n = p->tot_len - off;
        if (n > NET_BH_INLINE_PAYLOAD)
            n = NET_BH_INLINE_PAYLOAD;
        bh_stage_payload(&s->bh_ring, slot, e, p, (uint32_t)off, n);
        bh_ring_commit(&s->bh_ring);
        off += n;
    }
    return true;
}

/*
 * Schedule the per-socket bottom-half.  Called from lwIP callback context
 * with g_lwip_lock held; the pending flag array is accessed atomically so no
 * second lock is taken here.  net_bh_slot_mark() also bumps the global
 * pending count that gates the per-switch bottom-half scan.
 */
static void net_inet_bh_schedule(net_socket_t *s)
{
    if (!s)
        return;
    int idx = s->reg_idx;
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return;
    __atomic_store_n(&s->bh_pending, 1, __ATOMIC_RELEASE);
    net_bh_slot_mark(idx);
}

static void lwip_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return;

    net_bh_event_t *e;
    int slot = bh_ring_prepare(&s->bh_ring, &e);
    if (slot < 0) {
        pbuf_free(p);
        return;
    }

    net_lwip_ip_to_sockaddr(addr, port, e->addr, &e->addrlen);
#if LWIP_IPV6
    if (ip_current_is_v6()) {
        e->has_pktinfo = 1;
        e->pktinfo_ifindex = ip_current_input_netif() ?
            (uint32_t)netif_get_index(ip_current_input_netif()) : 0;
        memcpy(e->pktinfo_addr, ip6_current_dest_addr(), sizeof(e->pktinfo_addr));
        e->has_hoplimit = 1;
        e->hoplimit = IP6H_HOPLIM(ip6_current_header());
        e->has_tclass = 1;
        e->tclass = IP6H_TC(ip6_current_header());
    }
#endif
    size_t len = p->tot_len;
    if (len > NET_MAX_PAYLOAD)
        len = NET_MAX_PAYLOAD;
    bh_stage_payload(&s->bh_ring, slot, e, p, 0, len);

    bh_ring_commit(&s->bh_ring);
    net_inet_bh_schedule(s);
    pbuf_free(p);
}

static u8_t lwip_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return 0;

    /* Do not consume ICMP echo requests on raw ICMP sockets.  Let lwIP's
     * icmp_input() generate the echo reply so the socket can receive it.
     * The pbuf passed to the raw recv callback still contains the IP header. */
    if (s->protocol == IPPROTO_ICMP && p->tot_len > 0) {
        struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
        u16_t iphdr_hlen = IPH_HL_BYTES(iphdr);
        if (p->tot_len > iphdr_hlen) {
            uint8_t *payload = (uint8_t *)p->payload;
            if (payload[iphdr_hlen] == ICMP_ECHO)
                return 0;
        }
    }

    net_bh_event_t *e;
    int slot = bh_ring_prepare(&s->bh_ring, &e);
    if (slot < 0) {
        /* The ring is full, so the payload is dropped -- but this callback has
         * already taken ownership by freeing, and lwIP reads a non-zero return
         * as "I ate it" and leaves the pbuf alone.  Returning 0 here while
         * freeing made the caller free the same pbuf a second time, which is
         * the mirror image of the send-side double free fixed earlier. */
        a20_perf_count(A20_PERF_NET_BH_OVERFLOW);
        pbuf_free(p);
        return 1;
    }

    net_lwip_ip_to_sockaddr(addr, 0, e->addr, &e->addrlen);
#if LWIP_IPV6
    if (ip_current_is_v6()) {
        e->has_pktinfo = 1;
        e->pktinfo_ifindex = ip_current_input_netif() ?
            (uint32_t)netif_get_index(ip_current_input_netif()) : 0;
        memcpy(e->pktinfo_addr, ip6_current_dest_addr(), sizeof(e->pktinfo_addr));
        e->has_hoplimit = 1;
        e->hoplimit = IP6H_HOPLIM(ip6_current_header());
        e->has_tclass = 1;
        e->tclass = IP6H_TC(ip6_current_header());
    }
#endif
    size_t len = p->tot_len;
    if (len > NET_MAX_PAYLOAD)
        len = NET_MAX_PAYLOAD;
    bh_stage_payload(&s->bh_ring, slot, e, p, 0, len);

    bh_ring_commit(&s->bh_ring);
    net_inet_bh_schedule(s);
    pbuf_free(p);
    return 1;
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
    if (!p) {
        __atomic_store_n(&s->bh_closed, 1, __ATOMIC_RELEASE);
        net_inet_bh_schedule(s);
        return ERR_OK;
    }

    if (!net_inet_tcp_stage_payload(s, p)) {
        /* The ring is full.  ERR_MEM leaves the pbuf with lwIP, which parks it
         * in pcb->refused_data and offers it again once the application has
         * caught up, so it must not be freed here -- doing both handed lwIP a
         * segment it still owned and freed again on the retry. */
        return ERR_MEM;
    }
    net_inet_bh_schedule(s);
    pbuf_free(p);
    return ERR_OK;
}

static void lwip_tcp_err_cb(void *arg, err_t err)
{
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return;
    s->tcp = NULL;
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
 * Runs with g_lwip_lock held and must stay inside that contract: no
 * allocation, no net lock, no scheduler.  So the pcb is only
 * parked in the
 * listener's accept stage and the bottom half is scheduled; the child socket,
 * its registration and the accept-queue push happen there.  Returning ERR_OK
 * tells lwIP the pcb was accepted, so the connection stays ESTABLISHED and must
 * not be freed here -- if the stage is full the pcb has to be aborted, because
 * returning ERR_OK and dropping it would leak it with no owner.
 */
static err_t lwip_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        goto abort;
    if (err != ERR_OK)
        goto abort;

    net_accept_stage_t *st = &s->accept_stage;
    uint32_t head = __atomic_load_n(&st->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&st->tail, __ATOMIC_ACQUIRE);
    if ((head - tail) >= NET_ACCEPT_STAGE_SIZE) {
        __atomic_fetch_add(&st->dropped, 1, __ATOMIC_RELAXED);
        a20_perf_count(A20_PERF_NET_ACCEPT_DROP);
        goto abort;
    }

    /*
     * Service the pcb before publishing it -- see lwip_tcp_stage_recv_cb for
     * what happens otherwise.  The slot address becomes the pcb's callback_arg
     * and identifies the entry for every callback below, so the bottom half
     * may not recycle the slot while the pcb can still produce an event; it
     * hands the pcb to its child first and only then advances tail.
     */
    net_accept_stage_slot_t *c = &st->slots[head & (NET_ACCEPT_STAGE_SIZE - 1)];
    c->listener = s;
    c->pcb = newpcb;
    c->pending = NULL;
    c->fin = 0;
    c->dead = 0;
    c->error = 0;
    tcp_arg(newpcb, c);
    tcp_recv(newpcb, lwip_tcp_stage_recv_cb);
    tcp_err(newpcb, lwip_tcp_stage_err_cb);
    tcp_sent(newpcb, lwip_tcp_stage_sent_cb);

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&st->head, head + 1, __ATOMIC_RELAXED);
    a20_perf_count(A20_PERF_NET_ACCEPT_STAGED);
    net_inet_bh_schedule(s);
    return ERR_OK;

abort:
    if (newpcb)
        tcp_abort(newpcb);
    return ERR_OK;
}

/*
 * Release a stage slot's payload references and unbind it from its pcb.
 *
 * g_lwip_lock must be held: these are memp-owned pbuf references, and memp has
 * no internal locking.  `pcb` is left alone -- whether it still needs aborting
 * is the caller's decision, because it may already have been freed by lwIP.
 */
static void net_inet_accept_stage_slot_release(net_accept_stage_slot_t *c)
{
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

/*
 * Tear down every pcb still parked in a listener's accept stage.
 *
 * A staged pcb is an established connection in its own right: closing the
 * listening pcb does not reach it.  Leaving them behind would both leak those
 * connections and leave lwIP callbacks holding a pointer to a socket that is
 * about to be freed, so a listener going away takes its stage with it.
 *
 * g_lwip_lock must be held, which also means the producer is not running and
 * tail/head cannot move underneath this.
 */
static void net_inet_accept_stage_purge(net_socket_t *s)
{
    net_accept_stage_t *st = &s->accept_stage;
    uint32_t tail = __atomic_load_n(&st->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&st->head, __ATOMIC_ACQUIRE);
    for (uint32_t i = tail; i != head; i++) {
        net_accept_stage_slot_t *c = &st->slots[i & (NET_ACCEPT_STAGE_SIZE - 1)];
        struct tcp_pcb *pcb = c->pcb;
        bool dead = c->dead;
        net_inet_accept_stage_slot_release(c);
        if (pcb && !dead) {
            tcp_arg(pcb, NULL);
            tcp_abort(pcb);
        }
    }
    __atomic_store_n(&st->tail, head, __ATOMIC_RELEASE);
}

/*
 * Turn a staged, already-ESTABLISHED pcb into a registered child socket on the
 * listener's accept queue.  Runs from the bottom half, and takes the listener's
 * own bucket for the three short sections that touch it -- so this is where the
 * allocation and the bucket-protected push belong.
 *
 * It does *not* run inside the caller's critical section, because two of its
 * steps acquire locks of their own and must not be nested under a held bucket:
 * the pcb handoff takes a20_lwip_lock(), and net_register_socket_locked() takes
 * a socket-table shard.  A child is registered before it is pushed precisely so
 * net_inet_bh_schedule() below has a slot to mark; holding the listener's
 * bucket across the register would nest an arbitrary shard under a held one,
 * which is the ABBA the ascending shard order exists to prevent.
 *
 * A staged pcb is not guaranteed to still be alive when the drain gets to it:
 * lwIP abandons one on a reset whatever the socket layer does, and reports that
 * through the staged error callback, which has already run tcp_free() by the
 * time it fires.  So `dead` is honoured here by neither adopting nor aborting
 * -- touching either would be the double free this path is built to avoid.
 *
 * Returns false when every pcb seen was dropped; the caller must not touch them
 * again in that case.
 */
static bool net_inet_accept_stage_drain(net_socket_t *listener,
                                        proc_wake_q_t *wake_q)
{
    net_accept_stage_t *st = &listener->accept_stage;
    bool woke = false;
    uint32_t tail;
    uint32_t head;
    {
        /* Claim the ring.  The single g_net_lock made this a non-issue; the
         * drain has to drop the listener's bucket now, so the exclusion is
         * explicit.  A second drain arriving while one is in flight simply
         * returns: the ring is not emptied on this pass either way, and
         * net_inet_bh_schedule() below re-marks the listener's bottom half. */
        uint64_t cf = net_sock_lock(listener);
        if (st->drain_active) {
            net_sock_unlock(listener, cf);
            return false;
        }
        st->drain_active = 1;
        tail = __atomic_load_n(&st->tail, __ATOMIC_RELAXED);
        head = __atomic_load_n(&st->head, __ATOMIC_ACQUIRE);
        net_sock_unlock(listener, cf);
    }
    if (tail == head) {
        uint64_t cf = net_sock_lock(listener);
        st->drain_active = 0;
        net_sock_unlock(listener, cf);
        return false;
    }

    while (tail != head) {
        net_accept_stage_slot_t *c = &st->slots[tail & (NET_ACCEPT_STAGE_SIZE - 1)];

#if CONFIG_NET_RACE_DELAY_US
        /* Diagnostic amplifier; see net_profile.h.  Placed in the gap between
         * the dequeue and the a20_lwip_lock acquisition below on purpose -- that
         * gap is the window this exists to widen. */
        for (volatile uint32_t d = 0; d < CONFIG_NET_RACE_DELAY_US; d++)
            __asm__ __volatile__("" ::: "memory");
#endif

        /* Everything the child inherits is read (and nothing is written) under
         * the listener's own bucket, which is dropped again before the pcb
         * handoff.  The slot pointer stays valid across that drop: a staged pcb
         * is only recyclable once tail moves past it below, and no other
         * consumer drains this listener's ring. */
        net_socket_t *child = NULL;
        {
            uint64_t af = net_sock_lock(listener);
            if (listener->accept_count >= NET_MAX_QUEUE) {
                /* The application is not accepting.  Refusing here is the same
                 * choice the socket-layer connect path makes, and the peer sees a
                 * reset rather than a connection nobody will ever accept. */
                a20_perf_count(A20_PERF_NET_ACCEPT_DROP);
            } else if (!(child = net_socket_alloc())) {
                a20_perf_count(A20_PERF_NET_ALLOC_FAIL);
            }
            if (child) {
                /* The accepted connection inherits the listener's family.  It
                 * used to be hardcoded AF_INET, so a v6 connection that reached
                 * this drain was adopted into a v4 socket while holding a v6
                 * pcb -- every later address lookup on that fd read a 16-byte
                 * v6 sockaddr back as a 16-byte v4 one. */
                child->domain = listener->domain;
                child->type = SOCK_STREAM;
                child->protocol = listener->protocol;
                child->bound = 1;
                child->connected = 1;
                child->ever_connected = 1;
                child->nonblock = listener->nonblock;
                child->tcp_nodelay = listener->tcp_nodelay;
                /* Linux: an accepted socket inherits the listener's congestion
                 * control.  Without this a server that set TCP_CONGESTION on
                 * its listener would have every accepted connection silently
                 * fall back to the default, and getsockopt on the child would
                 * report an algorithm the caller never asked for. */
                child->tcp_congestion = listener->tcp_congestion;
                /* Same reasoning for the buffer ceilings: a listener that sized
                 * itself before accept() is sizing every connection it hands
                 * out, and a child that started from the defaults would report
                 * ceilings its parent never asked for.  net_inet_tcp_apply_options()
                 * below clamps them against the accepted pcb. */
                child->snd_buf = listener->snd_buf;
                child->rcv_buf = listener->rcv_buf;
                child->keepalive = listener->keepalive;
                child->keep_idle = listener->keep_idle;
                child->keep_intvl = listener->keep_intvl;
                child->keep_cnt = listener->keep_cnt;
                child->recv_timeout_ticks = listener->recv_timeout_ticks;
                child->send_timeout_ticks = listener->send_timeout_ticks;
                memcpy(child->local, listener->local, listener->local_len);
                child->local_len = listener->local_len;
            }
            net_sock_unlock(listener, af);
        }

        /* No net lock is held here, so the short a20_lwip_lock
         * acquisition for the pcb handoff runs on its own.  The staged ring's
         * producer holds a20_lwip_lock and never takes a net lock, so
         * dropping the listener's lock around this window cannot deadlock
         * against it -- and it keeps the rule that g_lwip_lock is never held
         * together with a net lock, which the single global lock broke. */
        uint64_t lf = a20_lwip_lock();
        struct tcp_pcb *pcb = c->pcb;
        bool adopted = false;
        bool fin = c->fin;
        bool payload = (c->pending != NULL);
#if CONFIG_NET_LANES > 1
        /*
         * The child's owning lane, taken from the pcb it is adopting rather
         * than from the listener.  This is the fix for the blocker
         * docs/net/net-lanes.md records under "阻塞": the two were not the same
         * number and neither of them was right for both ends.
         *
         * For a passive open the pcb copies the concrete destination address
         * out of the segment into local_ip (tcp_in.c, tcp_listen_input()), so
         * the pcb's owning lane is hash(that address, local_port).  The
         * listener's own lane is hash(its bound address, its port), which for
         * a specific bind is the same number but for a wildcard bind is not:
         * net_socket_lane_of_addr() hashes the zero address, so the listener
         * says net_lane_of(0, port) while its pcb is filed in the sentinel
         * bucket and the child it is about to hand out belongs to
         * hash(concrete_dst_ip, port).  telnetd binds INADDR_ANY, so this is
         * the common case, not the corner case.
         *
         * Declaring the child's lane before anything in this critical section
         * allocates is what makes it the lane the child's own work runs in:
         * net_inet_tcp_stage_payload() below and every later syscall on this
         * fd take the lane from the socket, and stage D dispatches inbound
         * packets to the pcb's owning lane.
         *
         * Guarded, not left to fold: at one lane the value is 0 either way,
         * but the store itself is not something the compiler may drop, and
         * the N=1 rule is that the preprocessed source is the old source.
         * `pcb` may be NULL on a slot whose producer already gave up, in
         * which case nothing is adopted and the listener's lane is the only
         * thing this section can meaningfully declare.
         */
        child->lane = pcb ? NET_PCB_LANE_OWNER_OF_PCB(pcb) : listener->lane;
        a20_lwip_lane_enter(child->lane);
#else
        a20_lwip_lane_enter(listener->lane);
#endif
        if (!c->dead && pcb && child) {
            child->tcp = pcb;
            /* Re-args the pcb to the child socket.  This is what makes the slot
             * recyclable: after it, no pcb can name `c` any more. */
            net_inet_tcp_apply_options(child, pcb);
            /* accept() reports the remote endpoint, which the accepted pcb
             * already carries.  Its local_ip is the address the peer reached us
             * on, which for a wildcard listener differs from the socket's own
             * bound address, so the listener's address is not a substitute. */
            net_lwip_ip_to_sockaddr(&pcb->remote_ip, 0,
                                    child->peer_addr, &child->peer_len);
            net_sockaddr_set_port(child->peer_addr, child->peer_len,
                                  (uint16_t)pcb->remote_port);
            /* Anything the peer sent before the handshake was adopted belongs
             * to the child now.  Staged here rather than dropped, because a
             * client that pipelines its first request into the handshake --
             * which is exactly what an HTTP client does -- would otherwise lose
             * it.  Copying into the ring is safe under a20_lwip_lock: a
             * TCP-sized segment always takes the inline arm, so memp is not
             * touched. */
            while (c->pending) {
                struct pbuf *p = c->pending;
                c->pending = p->next;
                p->next = NULL;
                if (!net_inet_tcp_stage_payload(child, p))
                    a20_perf_count(A20_PERF_NET_BH_OVERFLOW);
                pbuf_free(p);
            }
            if (fin) {
                /* The peer sent a FIN before we got here.  Handing the child a
                 * connection that is already at EOF, rather than one whose
                 * first read() blocks on a close that has already happened.
                 * `peer_closed`, not `closed`: this is the peer's half-close,
                 * and marking the child itself dead here would also make the
                 * staging loop below drop the payload the peer pipelined into
                 * the handshake -- which is exactly what an HTTP client does. */
                child->peer_closed = 1;
                __atomic_store_n(&child->bh_closed, 1, __ATOMIC_RELEASE);
            }
            adopted = true;
        } else if (!c->dead && pcb) {
            tcp_arg(pcb, NULL);
            tcp_abort(pcb);
        } else if (c->dead) {
            a20_perf_count(A20_PERF_NET_ACCEPT_DROP);
        }
        net_inet_accept_stage_slot_release(c);
        a20_lwip_unlock(lf);

        /* Published last, and with release semantics: the producer may reuse
         * this slot as soon as tail moves, so the re-arg above has to be
         * visible to it first. */
        __atomic_store_n(&st->tail, tail + 1, __ATOMIC_RELEASE);
        tail++;

        if (!adopted) {
            if (child)
                net_socket_free(child);
            continue;
        }

/* No net lock is held: net_register_socket_locked() takes a bucket
         * lock of its own and is documented as unsafe under a socket lock. */
        int rr = net_register_socket_locked(child);
        if (rr < 0) {
            uint64_t cf = a20_lwip_lock();
            tcp_abort(child->tcp);
            child->tcp = NULL;
            a20_lwip_unlock(cf);
            net_socket_free(child);
            a20_perf_count(A20_PERF_NET_ALLOC_FAIL);
            continue;
        }

        /* Second (and last) visit to the listener: the push and the wake.
         * The listener is re-validated because it may have been closed during the
         * unlocked window above. */
        bool queued = false;
        int qr = -ECONNREFUSED;
        uint64_t pf = net_sock_lock(listener);
        if (net_socket_is_live(listener)) {
            qr = net_accept_queue_push_locked(listener, child);
            if (qr >= 0) {
                queued = true;
                if (fin || payload) {
                    /* The ring and the EOF flag both landed on the child before it was
                     * registered, so nothing would otherwise ask its bottom half to
                     * look at them. */
                    net_inet_bh_schedule(child);
                }
                net_event_notify(listener, A20_EVENT_ACCEPT_READY, 0, 0);
                if (wait_queue_collect_one(&listener->accept_waitq, 0,
                                           PROC_WAKE_EVENT, wake_q))
                    woke = true;
            }
        }
        net_sock_unlock(listener, pf);
        if (queued) {
            a20_perf_count(A20_PERF_NET_ACCEPT_QUEUED);
            continue;
        }
        /* The child was registered a moment ago and is about to be dropped
         * again, so its slot goes back here -- with no net lock held, because
         * net_socket_unregister() takes a bucket lock and a bucket lock is never
         * taken under a socket lock.  The child is not yet marked closed, so a
         * table scan landing on it in this window can still find it queued; what
         * that scan would do with it is a plain enqueue attempt against a queue
         * the application never learns about, and the child's pcb is aborted
         * immediately below. */
        {
            uint64_t chf = net_sock_lock(child);
            child->closed = 1;
            net_sock_unlock(child, chf);
        }
        net_socket_unregister(child);
        uint64_t cf = a20_lwip_lock();
        tcp_abort(child->tcp);
        child->tcp = NULL;
        a20_lwip_unlock(cf);
        /* Two references: the creator's from net_socket_alloc() and the one
         * registration added a moment ago.  Both are gone, because the child
         * never reached the accept queue. */
        net_socket_free(child);
        net_socket_free(child);
        a20_perf_count(A20_PERF_NET_ACCEPT_DROP);
        continue;
    }
    {
        uint64_t cf = net_sock_lock(listener);
        st->drain_active = 0;
        net_sock_unlock(listener, cf);
    }
    return woke;
}

/*
 * True when s->tcp is a listening pcb rather than a connection.
 *
 * tcp_recv/tcp_sent/tcp_err all assert pcb->state != LISTEN, so every teardown
 * path has to ask the pcb what it is instead of assuming s->tcp is an
 * established connection.  A real LISTEN pcb exists whenever tcpmode is lwip;
 * before that, net_listen() dropped the bound pcb and s->tcp was always an
 * ordinary pcb, which is why this could be written as an unconditional call.
 */
static bool net_inet_tcp_pcb_is_listen(const net_socket_t *s)
{
    return s->tcp && s->tcp->state == LISTEN;
}

void net_tcp_close_pcb(net_socket_t *s)
{
    if (!s || !s->tcp)
        return;
    uint64_t flags = a20_lwip_lock();
    a20_lwip_lane_enter(s->lane);
    tcp_arg(s->tcp, NULL);
    if (net_inet_tcp_pcb_is_listen(s)) {
        tcp_accept(s->tcp, NULL);
        tcp_close(s->tcp);
        net_inet_accept_stage_purge(s);
    } else {
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
        tcp_sent(s->tcp, NULL);
        if (tcp_close(s->tcp) != ERR_OK)
            tcp_abort(s->tcp);
    }
    s->tcp = NULL;
    a20_lwip_unlock(flags);
}

void net_tcp_drop_pcb(net_socket_t *s)
{
    if (!s || !s->tcp)
        return;
    uint64_t flags = a20_lwip_lock();
    a20_lwip_lane_enter(s->lane);
    tcp_arg(s->tcp, NULL);
    if (net_inet_tcp_pcb_is_listen(s)) {
        tcp_close(s->tcp);
        net_inet_accept_stage_purge(s);
    } else {
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
        tcp_sent(s->tcp, NULL);
        if (tcp_close(s->tcp) != ERR_OK)
            tcp_abort(s->tcp);
    }
    s->tcp = NULL;
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

static unsigned
net_inet_bottom_half_process_socket_locked(net_socket_t *s,
                                           proc_wake_q_t *wake_q,
                                           bool *accept_pending)
{
    unsigned drain = 0;
    *accept_pending = s->listening &&
                      s->accept_stage.head != s->accept_stage.tail;
    if (__atomic_exchange_n(&s->bh_connected, 0, __ATOMIC_ACQUIRE)) {
        int err = __atomic_load_n(&s->bh_err_code, __ATOMIC_RELAXED);
        s->tcp_connecting = 0;
        s->tcp_err = err;
        if (err == ERR_OK) {
            s->connected = 1;
            s->ever_connected = 1;
            net_event_notify(s, A20_EVENT_CONNECTION, 0, 0);
            net_event_notify(s, A20_EVENT_WRITABLE, 0, 0);
        }
        if (net_wait_queue_collect_all_locked(
                &s->read_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_READ;
        if (net_wait_queue_collect_all_locked(
                &s->write_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_WRITE;
    }

/*
     * Take the peer-EOF and peer-error flags but do not act on them yet.  They
     * are applied *after* the ring has been drained, and that ordering is the
     * fix for a whole class of "the transfer never finishes": TCP delivers data
     * and the FIN that follows it in the same bottom-half pass whenever both
     * arrived together, and the ring's `if (!s->closed)` guard then threw away
     * bytes lwIP had already handed over.  read() answered 0 -- an EOF -- for a
     * connection that still owed the application its last segment.
     *
     * The same happened on reset, which is worse: an RST arriving behind the
     * peer's final data set bh_error, and the data went with it.
     *
     * So the flags are captured here and the ring is drained against the
     * *pre-existing* s->closed, which is the local close the guard was written
     * for.  A reader still sees the buffered bytes first and only then the EOF
     * or the error, which is what read(2) promises.
     *
     * Note that deferring is what makes the FIN fix below expressible at all:
     * acting on bh_closed before the staging loop is exactly what used to throw
     * the peer's last segment away.
     */
    int bh_error = __atomic_exchange_n(&s->bh_error, 0, __ATOMIC_ACQUIRE);
    int bh_error_code = bh_error ? __atomic_load_n(&s->bh_err_code,
                                                   __ATOMIC_RELAXED)
                                 : 0;
    int bh_closed = __atomic_exchange_n(&s->bh_closed, 0, __ATOMIC_ACQUIRE);

    for (;;) {
        net_bh_event_t *e = bh_ring_consume(&s->bh_ring);
        if (!e)
            break;
        if (!s->closed) {
            int queued;
            if (e->spill)
                queued = net_enqueue_msg_locked_pbuf(
                    s, e->spill, e->spill_off, e->len,
                    e->addrlen ? e->addr : NULL, e->addrlen, e);
            else
                queued = net_enqueue_msg_locked_meta(
                    s, e->data, e->len,
                    e->addrlen ? e->addr : NULL, e->addrlen, e);
            if (queued >= 0) {
                net_event_notify(s, A20_EVENT_READABLE, 0, 0);
                if (wake_q->count >= PROC_WAKE_Q_CAPACITY)
                    drain |= NET_BH_DRAIN_READ;
                else
                    (void)wait_queue_collect_one(
                        &s->read_waitq, 0, PROC_WAKE_EVENT, wake_q);
            }
        }
        bh_ring_consume_commit(&s->bh_ring);
    }

    if (bh_error) {
        s->tcp_connecting = 0;
        s->tcp_err = bh_error_code;
        s->closed = 1;
        net_event_notify(s, A20_EVENT_ERROR, (uint64_t)s->tcp_err, 0);
        net_event_notify(s, A20_EVENT_CLOSED, 0, 0);
        if (net_wait_queue_collect_all_locked(
                &s->read_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_READ;
        if (net_wait_queue_collect_all_locked(
                &s->write_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_WRITE;
    }

    if (bh_closed) {
        /*
         * The peer sent a FIN.  That is the peer half-closing, not this socket
         * dying, so it belongs in `peer_closed` -- which is the flag every read
         * path already treats as end-of-file (socket.c's recv checks
         * `s->closed || s->peer_closed` and returns 0 for either).
         *
         * Setting `closed` here instead was wrong twice over.  It made
         * net_socket_is_live() (socket_internal.h: `return s && !s->closed`)
         * report a perfectly healthy socket as dead, so the socket vanished
         * from /proc/net/tcp and was rejected by the ~39 is_live() call sites
         * the moment its peer finished writing -- a peer that merely stopped
         * writing killed the socket outright.  And, before the deferral above,
         * it ran *before* the staging loop, whose `if (!s->closed)` guard then
         * discarded every payload event still sitting in the ring.
         *
         * Applied after the drain, so the peer's final bytes are already queued
         * by the time this marks end-of-file: the reader sees the data first
         * and then the 0.  Both orderings are load-bearing, independently.
         */
        s->peer_closed = 1;
        net_event_notify(s, A20_EVENT_CLOSED, 0, 0);
        if (net_wait_queue_collect_all_locked(
                &s->read_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_READ;
        if (net_wait_queue_collect_all_locked(
                &s->write_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_WRITE;
    }

    if (__atomic_exchange_n(&s->bh_tx_wake, 0, __ATOMIC_ACQUIRE)) {
        net_event_notify(s, A20_EVENT_WRITABLE, 0, 0);
        if (net_wait_queue_collect_all_locked(
                &s->write_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_WRITE;
    }
    return drain;
}

void net_inet_bottom_half_process_socket(net_socket_t *s)
{
    if (!s)
        return;
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    unsigned drain = 0;
    bool accept_pending = false;
    uint64_t irq = net_sock_lock(s);
    if (net_socket_is_live(s)) {
        drain = net_inet_bottom_half_process_socket_locked(s, &wake_q,
                                                           &accept_pending);
        /* The drain below runs with no lock held, so the socket needs a
         * reference of our own: the registry's can be handed back by a
         * concurrent close() in the window between here and there. */
        if (accept_pending)
            net_socket_ref(s);
    }
    __atomic_store_n(&s->bh_pending, 0, __ATOMIC_RELEASE);
    net_bh_slot_clear(s->reg_idx);
    net_sock_unlock(s, irq);
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
}

void net_inet_bottom_half_process_all(void)
{
    /* One acquire load replaces a 1024-slot scan per context switch; a mark
     * racing after this check simply stays pending for the next switch. */
    if (!__atomic_load_n(&g_net_bh_pending_count, __ATOMIC_ACQUIRE))
        return;
    a20_perf_count(A20_PERF_NET_BH_RUNS);
    /* No bucket is held across the walk.  The pending bitmap is read without a
     * lock (it is a volatile int each producer sets atomically), a slot that is
     * actually pending is then re-fetched under its own bucket by
     * net_bucket_slot_ref(), and the work runs under that socket's lock.  The
     * old shape held one bucket across the whole inner loop, which is the exact
     * "take the whole shard set" pattern the lock rules forbid. */
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!__atomic_load_n(&g_net_bh_pending[i], __ATOMIC_ACQUIRE))
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
            if (net_socket_is_live(s)) {
                drain = net_inet_bottom_half_process_socket_locked(
                    s, &wake_q, &accept_pending);
                if (accept_pending)
                    net_socket_ref(s);
                __atomic_store_n(&s->bh_pending, 0, __ATOMIC_RELEASE);
            } else {
                __atomic_store_n(&s->bh_pending, 0, __ATOMIC_RELEASE);
            }
            net_bh_slot_clear(i);
            net_sock_unlock(s, irq);
        }
        /* Two references at most: the one net_bucket_slot_ref() took, plus the
         * one the accept drain below needs. */
        if (accept_pending && net_inet_accept_stage_drain(s, &wake_q))
            drain |= NET_BH_DRAIN_ACCEPT;
        if (accept_pending)
            net_socket_free(s);
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
void a20_net_cong_apply(struct tcp_pcb *pcb, uint8_t alg)
{
#if LWIP_TCP_CUBIC
    if (!pcb)
        return;
    if (alg == TCP_CONG_CUBIC) {
        if (pcb->cong_alg != TCP_CONG_CUBIC) {
            pcb->cong_alg = TCP_CONG_CUBIC;
            /* Switching algorithm on a live connection must not reset the
             * window or throw away how far it has grown, so the cubic state
             * starts empty rather than seeded: RFC 8312 4.8 already says what
             * to do when a connection enters congestion avoidance without a
             * congestion event behind it, and that is exactly this case --
             * W_max := the window as it is now, K := 0.  Seeding W_max from
             * cwnd here instead would be the same computation done in the
             * wrong place, and would leave the pcb's state depending on a
             * caller's choice of when to switch.
             *
             * Note this deliberately does NOT reproduce the RFC 2581 initial
             * window for an unhandshaken pcb, the way the old comment here
             * did: LWIP_TCP_CALC_INITIAL_CWND is a macro private to tcp_in.c,
             * and a second copy of it in the socket layer is a second thing to
             * drift out of step.  A pcb that has not finished its handshake
             * has not entered congestion avoidance at all, so tcp_cubic_on_ack
             * will not read this state until it has. */
            tcp_cubic_init(pcb);
        }
    } else {
        /* Back to Reno.  tcp_cubic_on_ack()/on_loss() are gated on cong_alg,
         * so the stale cubic state is simply never read again -- there is
         * nothing to unwind and no window to restore. */
        pcb->cong_alg = TCP_CONG_RENO;
    }
#else
    /* CUBIC is not compiled in.  On this configuration the setsockopt path
     * rejects every name but "reno" (see socket_control.c), so this can only
     * ever be called with the default and there is nothing to do. */
    LWIP_UNUSED_ARG(pcb);
    LWIP_UNUSED_ARG(alg);
#endif
}

/*
 * Apply SO_SNDBUF / SO_RCVBUF to an lwIP pcb.
 *
 * What each one actually does here, stated plainly because neither is Linux's
 * option and both are weaker than the name suggests:
 *
 *   SO_SNDBUF bounds how many bytes this socket may have sitting in its pcb's
 *   send queue at once.  net_inet_send_tcp() measures the queue depth against
 *   it and stops handing data to tcp_write() once the ceiling is reached, so
 *   the socket blocks (or reports a short write) instead of filling lwIP's
 *   TCP_SND_BUF.  It does NOT bound data in flight on the wire: that is
 *   congestion control's job, and bounding it here would fight the algorithm.
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
 * Must be called with g_lwip_lock held.
 */
void net_inet_tcp_buf_apply(net_socket_t *s, struct tcp_pcb *pcb)
{
    if (!s || !pcb)
        return;

    /* A LISTEN pcb has no receive window of its own -- lwIP creates the real
     * one in tcp_accept() -- and tcp_recved() asserts against it.  The socket
     * fields still carry the value, so the accepted child gets it when
     * net_inet_tcp_apply_options() runs on it. */
    if (pcb->state == LISTEN)
        return;

    if (s->snd_buf) {
        uint32_t snd = s->snd_buf;
        if (snd > (uint32_t)TCP_SND_BUF)
            snd = (uint32_t)TCP_SND_BUF;
        /* Written back, so getsockopt reports what is actually in force. */
        s->snd_buf = snd;
        /* pcb->snd_buf is AVAILABLE space, not capacity: tcp_write() subtracts
         * from it and an incoming ACK adds to it.  Assigning the ceiling would
         * therefore hand out fresh room every time the caller lowered SO_SNDBUF
         * below what is already queued.  Only ever lowering keeps the pcb's
         * arithmetic consistent; the matching cost is that a raised ceiling
         * takes effect on the next connection, because lwIP has no mechanism to
         * grow snd_buf once bytes are outstanding, and this does not invent one. */
        if (snd < pcb->snd_buf)
            pcb->snd_buf = (tcpwnd_size_t)snd;
    }

    if (s->rcv_buf) {
        /*
         * The ceiling is this build's configured TCP_WND, NOT
         * TCP_WND_MAX(pcb), and the scale bound uses the configured
         * TCP_RCV_SCALE, NOT pcb->rcv_scale.  Both of the pcb-relative forms
         * are wrong before the handshake, and wrong in a way that does not
         * recover: TCP_WND_MAX() is TCPWND16(TCP_WND) until the peer has
         * advertised window scaling, so on a socket created by socket() it
         * answers 65535 rather than 93440, and pcb->rcv_scale is still 0
         * until lwIP sends its first window-update option.  Clamping against
         * them would permanently pin every socket to 64 KiB from the instant
         * it was created, which is exactly what happened: an IPv6 loopback
         * transfer stopped completing in lwIP TCP mode, where a real pcb
         * exists, and not in fast mode, where one does not.
         */
        uint32_t rcv = s->rcv_buf;
        uint32_t ceiling = (uint32_t)TCP_WND;
        uint32_t scale_ceiling = (uint32_t)0xFFFFu << TCP_RCV_SCALE;
        if (scale_ceiling < ceiling)
            ceiling = scale_ceiling;
        if (rcv > ceiling)
            rcv = ceiling;
        s->rcv_buf = rcv;
        pcb->wnd_limit = (tcpwnd_size_t)rcv;
        /* Only a shrink needs announcing.  rcv_wnd can only be lowered here,
         * so a caller that raised the ceiling leaves nothing to say. */
        if (pcb->rcv_wnd > (tcpwnd_size_t)rcv) {
            pcb->rcv_wnd = (tcpwnd_size_t)rcv;
            /* And only when a window has been announced.  tcp_recved() on a
             * pcb that has not completed a handshake takes the
             * rcv_ann_wnd == 0 branch, inflates the window by a full segment
             * count, sets TF_ACK_NOW and calls tcp_output() -- which emits a
             * bare ACK for a connection that does not exist.  That stray
             * packet goes on the wire for every socket() in the system, and
             * a listener that has already bound the port sees it before any
             * handshake. */
            if (pcb->rcv_ann_wnd != 0)
                tcp_recved(pcb, 0);
        }
    }
}

/*
 * Apply the socket's TCP options and install its callbacks on an lwIP pcb.
 *
 * Shared by socket creation and by the accept path, which adopts a pcb lwIP
 * already handed it rather than allocating one.  Must be called with
 * g_lwip_lock held: every lwip_tcp_* call in here touches pcb state.
 */
void net_inet_tcp_apply_options(net_socket_t *s, struct tcp_pcb *pcb)
{
    if (s->tcp_nodelay)
        tcp_nagle_disable(pcb);
    a20_net_cong_apply(pcb, s->tcp_congestion);
    net_inet_tcp_buf_apply(s, pcb);
    if (s->keepalive)
        pcb->so_options |= SOF_KEEPALIVE;
    /*
     * SO_REUSEADDR has to reach the pcb, not just the socket record.  lwIP asks
     * ip_get_option(pcb, SOF_REUSEADDR) in tcp_bind(),
     * tcp_listen_with_backlog_and_err() and tcp_connect(), and nowhere else, so
     * a flag that stays in net_socket_t leaves the option inert: the TIME-WAIT
     * pcb of the previous connection keeps the local port reserved for
     * 2 * TCP_MSL and a restart on the same port fails with EADDRINUSE.  The
     * accepted child inherits it through SOF_INHERITED (tcp_in.c:
     * `npcb->so_options = pcb->so_options & SOF_INHERITED`), so this one call
     * site covers both the socket() path and the accept path.
     */
    if (s->reuseaddr)
        ip_set_option(pcb, SOF_REUSEADDR);
    if (s->keep_idle > 0)
        pcb->keep_idle = (u32_t)s->keep_idle * 1000U;
    if (s->keep_intvl > 0)
        pcb->keep_intvl = (u32_t)s->keep_intvl * 1000U;
    if (s->keep_cnt > 0)
        pcb->keep_cnt = (u32_t)s->keep_cnt;
    tcp_arg(pcb, s);
    tcp_recv(pcb, lwip_tcp_recv_cb);
    tcp_err(pcb, lwip_tcp_err_cb);
    tcp_sent(pcb, lwip_tcp_sent_cb);
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
int net_inet_tcp_listen(net_socket_t *s, int backlog)
{
    if (!s || !s->tcp)
        return -EINVAL;
    uint64_t flags = a20_lwip_lock();
    a20_lwip_lane_enter(s->lane);
    struct tcp_pcb *lpcb = tcp_listen_with_backlog(s->tcp, (u8_t)backlog);
    if (!lpcb) {
        a20_lwip_unlock(flags);
        return -ENOMEM;
    }
    /* tcp_listen_with_backlog() returns a new pcb; the bound one is consumed. */
    s->tcp = lpcb;
    tcp_arg(lpcb, s);
    tcp_accept(lpcb, lwip_tcp_accept_cb);
    a20_lwip_unlock(flags);
    return 0;
}

int net_inet_socket_init(net_socket_t *s)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;

    uint64_t flags = a20_lwip_lock();
    a20_lwip_lane_enter(s->lane);
    int ret = 0;
    if (s->type == SOCK_DGRAM) {
        s->udp = udp_new_ip_type(s->domain == AF_INET6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
        if (!s->udp) {
            ret = -ENOMEM;
            goto out;
        }
        udp_recv(s->udp, lwip_udp_recv_cb, s);
        goto out;
    }
    if (s->domain == AF_INET && s->type == SOCK_RAW) {
        s->raw = raw_new_ip_type(IPADDR_TYPE_V4, (u8_t)s->protocol);
        if (!s->raw) {
            ret = -ENOMEM;
            goto out;
        }
        raw_recv(s->raw, lwip_raw_recv_cb, s);
        goto out;
    }
    if (s->domain == AF_INET6 && s->type == SOCK_RAW) {
        s->raw = raw_new_ip_type(IPADDR_TYPE_V6, (u8_t)s->protocol);
        if (!s->raw) {
            ret = -ENOMEM;
            goto out;
        }
        raw_recv(s->raw, lwip_raw_recv_cb, s);
        goto out;
    }
    /*
     * SOCK_STREAM is dual-stack: lwIP's tcp_pcb carries an IPADDR_TYPE_V6 pcb
     * natively and takes the family from the ip_addr_t bind/connect hand it,
     * so one socket layer serves both.  This arm used to be AF_INET-only,
     * which is why net_listen() had no v6 path to convert -- an AF_INET6
     * socket() succeeded and then had s->tcp == NULL forever, so bind() could
     * not reach a pcb and listen() had nothing to turn into a LISTEN pcb.
     */
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        s->type == SOCK_STREAM) {
        s->tcp = tcp_new_ip_type(s->domain == AF_INET6 ? IPADDR_TYPE_V6
                                                       : IPADDR_TYPE_V4);
        if (!s->tcp) {
            ret = -ENOMEM;
            goto out;
        }
        net_inet_tcp_apply_options(s, s->tcp);
    }
out:
    a20_lwip_unlock(flags);
    return ret;
}

void net_inet_socket_destroy(net_socket_t *s)
{
    if (!s)
        return;
    uint64_t flags = a20_lwip_lock();
    a20_lwip_lane_enter(s->lane);
    if (s->udp) {
        udp_remove(s->udp);
        s->udp = NULL;
    }
    if (s->raw) {
        raw_remove(s->raw);
        s->raw = NULL;
    }
    if (s->tcp) {
        if (net_inet_tcp_pcb_is_listen(s)) {
            /* tcp_abort() and tcp_abandon() both assert on a LISTEN pcb, and
             * tcp_close() is the only teardown lwIP accepts for a listener.  It
             * also reaps the children the listener still owns, which is what a
             * listener with queued accepts must do rather than orphaning them. */
            tcp_close(s->tcp);
            /* ...but not the connections still parked in the accept stage,
             * which no lwIP list owns.  Their slots point back at this socket,
             * so they have to go before it does. */
            net_inet_accept_stage_purge(s);
        } else {
/* Graceful close, not abort.  close() on a stream socket has to
             * flush what the application already handed to write() and then
             * send a FIN; tcp_abort() is tcp_abandon(pcb, 1), which sends an
             * RST instead.  An RST makes the peer throw away data it has
             * already received, so `write(); close()` lost the tail of every
             * message on this path -- the single most common shape a
             * request/response server uses to reply.
             *
             * That tail loss is also why the ordering fix in
             * net_inet_bottom_half_process_socket_locked() exists: the FIN and
             * the last segment routinely arrive in the same bottom-half pass,
             * so the reader has to see the data before the EOF.
             *
             * It survived because the loss is a race against the peer's read,
             * not a deterministic one: a workload that sleeps before closing
             * passes, and the same workload with an immediate close sees a
             * short read.  The loopback probe that found it wrote 4000 B and
             * closed; with an 8 s pause in between the same probe passed.
             *
             * The same shape net_tcp_close_pcb() uses, deliberately: callbacks
             * are unbound first so the teardown cannot re-enter the socket
             * layer, tcp_close() sends the FIN and hands the pcb to lwIP's
             * closing states, and abort is the documented fallback for the one
             * case where tcp_close() cannot take the pcb (ERR_MEM with no room
             * for the FIN and no CLOSEPEND retry).  net_tcp_close_pcb()
             * already closes gracefully for the connect-timeout paths; this arm
             * had disagreed with it about the same pcb. */
            tcp_arg(s->tcp, NULL);
            tcp_recv(s->tcp, NULL);
            tcp_err(s->tcp, NULL);
            tcp_sent(s->tcp, NULL);
            if (tcp_close(s->tcp) != ERR_OK)
                tcp_abort(s->tcp);
        }
        s->tcp = NULL;
    }
    /* Release any spill reference the ring still holds.  This runs under
     * g_lwip_lock, which is the only context where memp may be touched, and it
     * has to happen here because the ring dies with the socket. */
    for (int i = 0; i < NET_BH_RING_SIZE; i++) {
        if (s->bh_ring.owned[i]) {
            pbuf_free(s->bh_ring.owned[i]);
            s->bh_ring.owned[i] = NULL;
        }
    }
    a20_lwip_unlock(flags);
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
int net_inet_bind_pcb(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;
    if (s->udp) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        err_t e = udp_bind(s->udp, &ip, port);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -EADDRINUSE;
    }
    if (s->raw) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        err_t e = raw_bind(s->raw, &ip);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -EADDRINUSE;
    }
    if (s->tcp) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        err_t e = tcp_bind(s->tcp, &ip, port);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -EADDRINUSE;
    }
    return 0;
}

static int net_inet_connect_stream(net_socket_t *s, const void *addr, size_t addrlen,
                                   const void *connect_addr, size_t peer_len)
{
    {
        uint64_t irq = net_sock_lock(s);
        if (!net_socket_is_live(s)) {
            net_sock_unlock(s, irq);
            return -ENOTSOCK;
        }
        if (!s->bound)
            net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
        net_sock_unlock(s, irq);
    }

    net_socket_t *child = net_socket_alloc();
    if (!child)
        return -ENOMEM;
    uint16_t connect_port = 0;
    net_sockaddr_port(connect_addr, peer_len, &connect_port);
    /* A20_TCP_PATH_LWIP exists so a load can be driven through the real stack;
     * see the a20_tcp_path_t comment.  The shortcut is only taken when it is
     * both the configured mode and applicable. */
    int local_target = g_a20_tcp_path == A20_TCP_PATH_FAST &&
                       net_sockaddr_is_local_target(connect_addr, peer_len);
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    /* Resolved outside every other lock: the search walks the table one bucket
     * at a time and comes back with a reference, which is what lets s's lock
     * be dropped before the pairs below. */
    net_socket_t *listener =
        local_target ? net_find_stream_listener(s, connect_port) : NULL;
    if (listener) {
        /* Everything the child inherits is read under the listener's own lock
         * alone.  s->local is stable after bind -- a second bind() is refused
         * before it can rewrite it -- so reading it here unlocked is safe and
         * keeping this section single-socket avoids a pointless pair.  The child
         * is not yet registered and nothing else can name it, so filling it in
         * needs no lock of its own. */
        uint64_t lf = net_sock_lock(listener);
        bool gate = net_socket_is_live(listener) && listener->listening &&
                    listener->accept_count < NET_MAX_QUEUE;
        if (gate) {
            child->domain = listener->domain;
            child->type = SOCK_STREAM;
            child->protocol = s->protocol;
            child->bound = 1;
            child->connected = 1;
            child->ever_connected = 1;
            memcpy(child->local, listener->local, listener->local_len);
            child->local_len = listener->local_len;
            memcpy(child->peer_addr, s->local, s->local_len);
            child->peer_len = s->local_len;
        }
        net_sock_unlock(listener, lf);
        if (gate) {
            /* No net lock is held: net_register_socket_locked() takes a
             * bucket lock of its own and must not run under a socket lock. */
            int rr = net_register_socket_locked(child);
            if (rr < 0) {
                net_socket_free(listener);
                net_socket_free(child);
                return rr;
            }
            {
                /* Second visit to both sockets: wire the pair and push.  Both
                 * may have been closed during the unlocked window, so the
                 * liveness of each is re-checked here. */
                net_sock_pair_t pair = net_sock_lock2(s, listener);
                int qr = -ECONNREFUSED;
                if (net_socket_is_live(s) &&
                    net_socket_is_live(listener))
                    qr = net_accept_queue_push_locked(listener, child);
                if (qr >= 0) {
                    child->peer = s;
                    s->peer = child;
                    s->connected = 1;
                    s->ever_connected = 1;
                    s->local_tcp = 1;
                    child->local_tcp = 1;
                    ktrace_net("[NET] connect: pushed child to listener accept queue\n");
                    net_event_notify(listener, A20_EVENT_ACCEPT_READY, 0, 0);
                    (void)wait_queue_collect_one(
                        &listener->accept_waitq, 0, PROC_WAKE_EVENT, &wake_q);
                    net_sock_unlock2(pair);
                    net_socket_free(listener);
                    (void)proc_wake_q_flush(&wake_q);
                    net_tcp_drop_pcb(s);
                    ktrace_net("[NET] connect: local TCP connect done\n");
                    return 0;
                }
                s->connected = 0;
                s->peer = NULL;
                /* child is not in the pair -- it has just been registered and
                 * nothing else can name it yet, so its own fields are only
                 * written here.  Its slot goes back below with no net lock
                 * held, because net_socket_unregister() takes a bucket lock. */
                child->closed = 1;
                net_sock_unlock2(pair);
                net_socket_unregister(child);
                /* The registry's reference, then the creator's. */
                net_socket_free(child);
                net_socket_free(child);
                net_socket_free(listener);
                (void)proc_wake_q_flush(&wake_q);
                return qr;
            }
        }
        net_socket_free(listener);
    }
    net_socket_free(child);

    /* The stack path.  s->tcp exists for both families now
     * (net_inet_socket_init), and tcp_connect() reads the family off the
     * ip_addr_t, so the AF_INET-only guard this used to carry just refused
     * every v6 connect outright -- with -ECONNREFUSED, which is a lie: the
     * address was fine, the kernel simply had no path for it. */
    if (!s->tcp) {
        s->connected = 0;
        return -ECONNREFUSED;
    }

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
    a20_lwip_lane_enter(s->lane);
    err_t e = tcp_connect(s->tcp, &ip, port, lwip_tcp_connected_cb);
    a20_lwip_unlock(lwip_flags);
    if (e != ERR_OK) {
        s->tcp_connecting = 0;
        return -ENETUNREACH;
    }
    if (s->nonblock)
        return -EINPROGRESS;

    uint64_t timeout = s->send_timeout_ticks ? s->send_timeout_ticks : NET_CONNECT_TIMEOUT_TICKS;
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

int net_inet_connect(net_socket_t *s, const void *addr, size_t addrlen,
                     const void *connect_addr, size_t peer_len)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;
    if (s->udp && s->domain == AF_INET6)
        return 0;
    if (s->udp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        err_t e = udp_connect(s->udp, &ip, port);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -ENETUNREACH;
    }
    if (s->raw && s->domain == AF_INET) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        err_t e = raw_connect(s->raw, &ip);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -ENETUNREACH;
    }
    if (s->raw && s->domain == AF_INET6) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        err_t e = raw_connect(s->raw, &ip);
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
 * Must be called with g_lwip_lock held: it touches pcb state only, and it must
 * never allocate (see docs/net/network-lock-contract.md).
 */
#define NET_IP_TTL_DEFAULT 64

/* The values a packet from this socket would actually carry.  getsockopt has
 * to report these rather than the stored fields, because "never set" and "set
 * to 0" are different requests and only the effective value is observable on
 * the wire.  Both callers read the fields without a net lock,
 * matching the
 * per-socket option stores in socket_control.c; the values are single bytes,
 * so a concurrent setsockopt can only change the value read, never tear it. */
void net_inet_ip_effective(net_socket_t *s, uint8_t *ttl, uint8_t *tos,
                           uint8_t *mc_ttl)
{
    if (ttl)
        *ttl = s->ip_ttl_set ? s->ip_ttl : NET_IP_TTL_DEFAULT;
    if (tos)
        *tos = s->ip_tos_set ? s->ip_tos : 0;
    /* mc_ttl is a hop count where 0 means 1; the IP header field must be >= 1. */
    if (mc_ttl)
        *mc_ttl = s->mc_ttl ? s->mc_ttl : 1;
}

static void net_inet_ip_opts_apply_locked(net_socket_t *s)
{
    uint8_t ttl, tos, mc_ttl;
    net_inet_ip_effective(s, &ttl, &tos, &mc_ttl);
    if (s->udp) {
        s->udp->ttl = ttl;
        s->udp->tos = tos;
        udp_set_multicast_ttl(s->udp, mc_ttl);
        if (s->mc_loop)
            udp_set_flags(s->udp, UDP_FLAGS_MULTICAST_LOOP);
        else
            udp_clear_flags(s->udp, UDP_FLAGS_MULTICAST_LOOP);
    }
    if (s->raw) {
        s->raw->ttl = ttl;
        s->raw->tos = tos;
        raw_set_multicast_ttl(s->raw, mc_ttl);
        if (s->mc_loop)
            raw_set_flags(s->raw, RAW_FLAGS_MULTICAST_LOOP);
        else
            raw_clear_flags(s->raw, RAW_FLAGS_MULTICAST_LOOP);
    }
    if (s->tcp) {
        s->tcp->ttl = ttl;
        s->tcp->tos = tos;
    }
}

/* Lock-taking wrapper for the setsockopt path. */
void net_inet_ip_opts_apply(net_socket_t *s)
{
    if (!s)
        return;
    uint64_t flags = a20_lwip_lock();
    net_inet_ip_opts_apply_locked(s);
    a20_lwip_unlock(flags);
}

static int net_inet_send_udp(net_socket_t *s, const void *buf, size_t len,
                             int flags, const void *addr, size_t addrlen)
{
    if (!s->bound) {
        uint16_t port = net_alloc_ephemeral_port_locked();
        if (s->domain == AF_INET6) {
            net_sockaddr_in6_t in6;
            memset(&in6, 0, sizeof(in6));
            in6.sin6_family = AF_INET6;
            in6.sin6_port = port;
            in6.sin6_addr[15] = 1;
            memcpy(s->local, &in6, sizeof(in6));
            s->local_len = sizeof(in6);
            s->bound = 1;
        } else {
            net_sockaddr_loopback(s, port);
            ip_addr_t any;
            ip_addr_set_zero_ip4(&any);
            uint64_t flags = a20_lwip_lock();
            a20_lwip_lane_enter(s->lane);
            udp_bind(s->udp, &any, net_ntohs(port));
            a20_lwip_unlock(flags);
        }
    }
    /* Same shape as the INET stream send: the destination is resolved first, with
     * no lock held, because net_find_udp_dst() walks the table one bucket at a
     * time and returns a reference.  s's own fields are copied out under s's
     * lock, since the enqueue below runs after that lock is dropped. */
    uint64_t src_local_len = 0;
    uint8_t src_local[NET_SOCKADDR_MAX];
    int dontwait = 0;
    uint64_t send_timeout = 0;
    net_socket_t *local_dst = NULL;
    const void *dst_addr = addr;
    size_t dst_len = addrlen;
    uint8_t peer_copy[NET_SOCKADDR_MAX];
    bool had_peer = false;
    {
        uint64_t sf = net_sock_lock(s);
        if (!net_socket_is_live(s)) {
            net_sock_unlock(s, sf);
            return -ENOTSOCK;
        }
        if (!dst_addr && s->connected) {
            dst_len = s->peer_len;
            memcpy(peer_copy, s->peer_addr, dst_len);
            dst_addr = peer_copy;
        }
        src_local_len = s->local_len;
        memcpy(src_local, s->local, src_local_len);
        /* Sampled under s's lock, so it cannot be freed between the test and
         * the net_socket_ref() below. */
        net_socket_t *peer = s->peer;
        if (peer && net_socket_is_live(peer))
            local_dst = net_socket_ref(peer);
        dontwait = s->nonblock || ((flags & MSG_DONTWAIT) != 0);
        send_timeout = s->send_timeout_ticks;
        had_peer = (peer != NULL);
        net_sock_unlock(s, sf);
    }
    if (!local_dst && had_peer) {
        /* The back-pointer is stale: drop it and fall back to a lookup. */
        uint64_t sf = net_sock_lock(s);
        if (s->peer) s->peer = NULL;
        net_sock_unlock(s, sf);
    }
    if (!local_dst && dst_addr)
        local_dst = net_find_udp_dst(s, dst_addr, dst_len);
    if (local_dst) {
        net_sock_pair_t pair = net_sock_lock2(s, local_dst);
        if (!net_socket_is_live(local_dst)) {
            net_sock_unlock2(pair);
            net_socket_free(local_dst);
            return dst_addr ? -ECONNREFUSED : -EDESTADDRREQ;
        }
        if (local_dst->rx_count >= NET_MAX_QUEUE && !dontwait) {
            net_sock_unlock2(pair);
            int br = net_enqueue_msg_blocking(s, local_dst, buf, len,
                                              src_local, src_local_len,
                                              dontwait, send_timeout);
            net_socket_free(local_dst);
            return br;
        }
        int rr = net_enqueue_msg_locked(local_dst, buf, len, src_local,
                                        src_local_len);
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        if (rr >= 0)
            (void)wait_queue_collect_one(
                &local_dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
        net_sock_unlock2(pair);
        (void)proc_wake_q_flush(&wake_q);
        net_socket_free(local_dst);
        return rr;
    }

    if (s->domain == AF_INET6)
        return dst_addr ? -ECONNREFUSED : -EDESTADDRREQ;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p)
        return -ENOMEM;
    pbuf_take(p, buf, (u16_t)len);
    ip_addr_t ip;
    uint16_t port = 0;
    err_t e;
    if (addr) {
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0) {
            pbuf_free(p);
            return r;
        }
    }
    uint64_t lwip_flags = a20_lwip_lock();
    a20_lwip_lane_enter(s->lane);
    net_inet_ip_opts_apply_locked(s);
    if (addr) {
        e = udp_sendto(s->udp, p, &ip, port);
    } else if (s->connected) {
        e = udp_send(s->udp, p);
    } else {
        a20_lwip_unlock(lwip_flags);
        pbuf_free(p);
        return -EDESTADDRREQ;
    }
    /* udp_sendto()/udp_send() take ownership of the pbuf and free it on every
     * path, including error, so it must not be freed again here. */
    a20_lwip_poll_locked();
    a20_lwip_unlock(lwip_flags);
    return e == ERR_OK ? (int)len : -EIO;
}

static int net_inet_send_raw(net_socket_t *s, const void *buf, size_t len,
                             const void *addr, size_t addrlen)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p)
        return -ENOMEM;
    pbuf_take(p, buf, (u16_t)len);
    ip_addr_t ip;
    err_t e;
    if (addr) {
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0) {
            pbuf_free(p);
            return r;
        }
    }
    uint64_t lwip_flags = a20_lwip_lock();
    a20_lwip_lane_enter(s->lane);
    net_inet_ip_opts_apply_locked(s);
    if (addr) {
        e = raw_sendto(s->raw, p, &ip);
    } else if (s->connected) {
        e = raw_send(s->raw, p);
    } else {
        a20_lwip_unlock(lwip_flags);
        pbuf_free(p);
        return -EDESTADDRREQ;
    }
    /* raw_sendto()/raw_send() take ownership of the pbuf and free it on every
     * path, including error, so it must not be freed again here. */
    a20_lwip_poll_locked();
    a20_lwip_unlock(lwip_flags);
    return e == ERR_OK ? (int)len : -EIO;
}

static int net_inet_send_tcp(net_socket_t *s, const void *buf, size_t len)
{
    /* Linux ABI: writes on a once-established endpoint that died report EPIPE */
    if (!s->connected || s->closed)
        return s->ever_connected ? -EPIPE : -ENOTCONN;
    if (s->shut_wr)
        return -ENOTCONN;
    if (s->local_tcp) {
        /* Two sockets, ascending by address; net_enqueue_msg_blocking()
         * takes that same pair itself, so this section only resolves `dst` and
         * checks it is still live.  s's own send options are copied out here
         * because the enqueue below runs with no lock on s. */
        net_socket_t *dst = NULL;
        uint64_t src_local_len = 0;
        uint8_t src_local[NET_SOCKADDR_MAX];
        bool nonblock = false;
        uint64_t send_timeout = 0;
        uint64_t irq = net_sock_lock(s);
        if (!net_socket_is_live(s)) {
            net_sock_unlock(s, irq);
            return -ENOTSOCK;
        }
        src_local_len = s->local_len;
        memcpy(src_local, s->local, src_local_len);
        /* Sampled under s's lock, which is what keeps it from being freed
         * before the reference is taken. */
        if (s->peer)
            dst = net_socket_ref(s->peer);
        nonblock = s->nonblock;
        send_timeout = s->send_timeout_ticks;
        net_sock_unlock(s, irq);
        if (!dst)
            return -ENOTCONN;
        {
            net_sock_pair_t pair = net_sock_lock2(s, dst);
            bool usable = net_socket_is_live(dst);
            net_sock_unlock2(pair);
            if (!usable) {
                net_socket_free(dst);
                return -ENOTCONN;
            }
        }
        int rv = net_enqueue_msg_blocking(s, dst, buf, len,
                                          src_local, src_local_len,
                                          nonblock, send_timeout);
        net_socket_free(dst);
        return rv;
    }
    size_t sent = 0;
    uint64_t start = timer_get_ticks();
    while (sent < len) {
        /*
         * One acquisition per iteration.  a20_lwip_poll_locked() is the
         * progress half of a20_lwip_poll() and is what the UDP and RAW send
         * paths already run inside their own critical section; calling the
         * wrapper here took and released g_lwip_lock twice per iteration and
         * ran the whole-stack pass twice.  A 4 MiB write iterates about 64
         * times against a 64 KiB send buffer, so that was 128 full-stack
         * passes where 64 suffice.
         *
         * The bottom halves deliberately do not run per iteration: they take
         * socket locks, which are never held together with g_lwip_lock.  One
         * drain after the loop covers the same ground.
         */
        uint64_t lwip_flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        a20_lwip_poll_locked();
        int tcp_alive = s->tcp && !s->closed && s->connected;
        /*
         * Room under the socket's own SO_SNDBUF ceiling, not just under the
         * pcb's capacity.  pcb->snd_buf is AVAILABLE space (tcp_write
         * subtracts, an incoming ACK adds), so the queue depth is
         * TCP_SND_BUF minus what is left of it; comparing that against
         * s->snd_buf is what makes the option mean anything.  Without it the
         * loop would keep filling lwIP's TCP_SND_BUF whatever the caller asked
         * for, and SO_SNDBUF would be the no-op it was before.
         *
         * Reading pcb->snd_buf directly rather than through tcp_sndbuf() is
         * deliberate: that macro is TCPWND16(), upstream's 16-bit accessor, so
         * on this port it reports at most 65535 even though TCP_SND_BUF is
         * 93440.  Going through it would quietly cap every send at 64 KiB.
         */
        uint32_t room32 = 0;
        if (tcp_alive) {
            tcpwnd_size_t avail = s->tcp->snd_buf;
            uint32_t queued = (uint32_t)TCP_SND_BUF > (uint32_t)avail
                              ? (uint32_t)(TCP_SND_BUF - avail) : 0;
            uint32_t ceiling = s->snd_buf ? s->snd_buf : (uint32_t)TCP_SND_BUF;
            room32 = (ceiling > queued) ? (ceiling - queued) : 0;
            if (room32 > 0xffff)
                room32 = 0xffff;
        }
        if (!tcp_alive) {
            a20_lwip_unlock(lwip_flags);
            return sent ? (int)sent : -EPIPE;
        }
        net_inet_ip_opts_apply_locked(s);
        if (room32 == 0) {
            a20_lwip_unlock(lwip_flags);
            if (sent || s->nonblock)
                return sent ? (int)sent : -EAGAIN;
            task_t *cur = proc_current();
            if (!cur)
                return -EAGAIN;
            if (net_task_has_unblocked_signal(cur))
                return -ERESTARTSYS;
            if (net_socket_wait_expired(s, start, 1))
                return -EAGAIN;
            uint64_t deadline = s->send_timeout_ticks ?
                                start + s->send_timeout_ticks : 0;
            proc_wait_token_t token =
                proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
            if (!token.task)
                return -EAGAIN;

            wait_queue_entry_t entry = {0};
            uint64_t irq = net_sock_lock(s);
            bool linked =
                wait_queue_link(&s->write_waitq, &entry, token, 0);
            net_sock_unlock(s, irq);
            uint64_t room_flags = a20_lwip_lock();
            int room_now = 0;
            if (s->tcp && !s->closed && s->connected) {
                tcpwnd_size_t avail = s->tcp->snd_buf;
                uint32_t queued = (uint32_t)TCP_SND_BUF > (uint32_t)avail
                                  ? (uint32_t)(TCP_SND_BUF - avail) : 0;
                uint32_t ceiling = s->snd_buf ? s->snd_buf : (uint32_t)TCP_SND_BUF;
                room_now = (ceiling > queued) && (ceiling - queued) > 0;
            }
            a20_lwip_unlock(room_flags);
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
            if (reason == PROC_WAKE_TIMEOUT)
                return -EAGAIN;
            continue;
        }
        /* room32 > 0: merge the sndbuf check with write/output under one lock
         * acquisition.  The lock above measured room and confirmed liveness;
         * re-verify liveness under the same lock before writing, because
         * tcp_write() would otherwise have to fail on a dead pcb. */
        size_t n = len - sent;
        if (n > room32)
            n = room32;
        if (n > 0xffff)
            n = 0xffff;
        if (!s->tcp || s->closed || !s->connected) {
            a20_lwip_unlock(lwip_flags);
            return sent ? (int)sent : -EPIPE;
        }
        err_t e = tcp_write(s->tcp, (const uint8_t *)buf + sent,
                            (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK) {
            a20_lwip_unlock(lwip_flags);
            return sent ? (int)sent : -EIO;
        }
        e = tcp_output(s->tcp);
        if (e != ERR_OK) {
            a20_lwip_unlock(lwip_flags);
            return sent ? (int)sent : -EIO;
        }
        a20_lwip_unlock(lwip_flags);
        sent += n;
    }
    /* Outside g_lwip_lock, for the same reason the loop does not run them.
     * The error returns above skip this, which is safe because sched() runs
     * both bottom-halves before picking the next task. */
    net_inet_bottom_half_process_all();
    net_packet_bottom_half_process();
    return (int)sent;
}

int net_inet_sendto(net_socket_t *s, const void *buf, size_t len,
                    int flags, const void *addr, size_t addrlen)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return -EAFNOSUPPORT;
    if (s->udp)
        return net_inet_send_udp(s, buf, len, flags, addr, addrlen);
    if (s->raw)
        return net_inet_send_raw(s, buf, len, addr, addrlen);
    if (s->tcp)
        return net_inet_send_tcp(s, buf, len);
    return -EOPNOTSUPP;
}

void net_inet_accept_child_ready(net_socket_t *s)
{
    if (s && s->tcp) {
        uint64_t flags = a20_lwip_lock();
        /* tcp_backlog_accepted() can emit the handshake ACK, and an ACK that
         * does not fit in the send window is queued, which is a PBUF_POOL
         * allocation. */
        a20_lwip_lane_enter(s->lane);
        tcp_backlog_accepted(s->tcp);
        a20_lwip_unlock(flags);
    }
}

void net_tcp_recved(net_socket_t *s, size_t len) {
    if (s && s->tcp && len > 0) {
        uint64_t flags = a20_lwip_lock();
        a20_lwip_lane_enter(s->lane);
        while (len > 0) {
            uint16_t n = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
            tcp_recved(s->tcp, n);
            len -= n;
        }
        a20_lwip_unlock(flags);
    }
}
