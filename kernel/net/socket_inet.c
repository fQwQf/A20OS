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
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/prot/icmp.h"

static uint16_t g_next_ephemeral = 49152;

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
    uint16_t p = g_next_ephemeral++;
    if (g_next_ephemeral < 49152)
        g_next_ephemeral = 49152;
    return net_htons(p);
}

void net_sockaddr_loopback(net_socket_t *s, uint16_t port)
{
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

static net_socket_t *net_find_stream_listener_locked(net_socket_t *s,
                                                     uint16_t port)
{
    net_socket_t *first = NULL;
    net_socket_t *best = NULL;
    int best_load = NET_MAX_QUEUE + 1;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *cand = g_sockets[i];
        if (!cand || !cand->bound || !cand->listening ||
            cand->type != SOCK_STREAM)
            continue;
        if (!net_inet_domains_overlap(cand->domain, s->domain))
            continue;
        uint16_t cand_port = 0;
        if (net_sockaddr_port(cand->local, cand->local_len, &cand_port) != 0 ||
            cand_port != port)
            continue;
        if (!first)
            first = cand;
        if (cand->accept_count < NET_MAX_QUEUE &&
            cand->accept_count < best_load) {
            best = cand;
            best_load = cand->accept_count;
        }
    }
    if (best)
        return best;
    return first;
}

static net_socket_t *net_find_udp_dst_locked(net_socket_t *src,
                                             const void *dst_addr,
                                             size_t dst_len)
{
    if (!src || !dst_addr)
        return NULL;
    uint16_t dst_port = 0;
    if (net_sockaddr_port(dst_addr, dst_len, &dst_port) < 0)
        return NULL;

    net_socket_t *fallback = NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *cand = g_sockets[i];
        if (!cand || cand == src || !cand->bound || cand->type != SOCK_DGRAM)
            continue;
        if (!net_inet_domains_overlap(cand->domain, src->domain))
            continue;
        uint16_t cand_port = 0;
        if (net_sockaddr_port(cand->local, cand->local_len, &cand_port) < 0 ||
            cand_port != dst_port)
            continue;
        if (cand->connected) {
            if (net_sockaddr_port_equal(cand->peer_addr, cand->peer_len,
                                        src->local, src->local_len))
                return cand;
            continue;
        }
        if (!fallback)
            fallback = cand;
    }
    return fallback;
}

int net_sockaddr_to_lwip_ip(const void *addr, size_t len,
                            ip_addr_t *ip, uint16_t *port)
{
    if (!addr || !ip || len < sizeof(net_sockaddr_in_t))
        return -EINVAL;
    const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)addr;
    if (in->sin_family != AF_INET)
        return -EOPNOTSUPP;
    ip_addr_set_ip4_u32(ip, in->sin_addr);
    if (port)
        *port = net_ntohs(in->sin_port);
    return 0;
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


int net_lwip_ip_to_sockaddr(const ip_addr_t *ip, uint16_t port,
                            uint8_t out[NET_SOCKADDR_MAX], size_t *outlen)
{
    if (!out || !outlen || !IP_IS_V4(ip))
        return -EINVAL;
    net_sockaddr_in_t in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = net_htons(port);
    in.sin_addr = ip_2_ip4(ip)->addr;
    memcpy(out, &in, sizeof(in));
    *outlen = sizeof(in);
    return 0;
}

/*
 * Bottom-half ring helpers.
 *
 * The ring is single-producer (lwIP callback running under g_lwip_lock,
 * interrupts disabled) and single-consumer (net_inet_bottom_half_process_socket
 * running under g_net_lock only).  All index updates use __atomic intrinsics so
 * the ring is safe on SMP without holding both locks at once.
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
 * the accept callback: no allocation, no g_net_lock, no scheduler.
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
 * allocation, no g_net_lock, no scheduler.  So the pcb is only parked in the
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
 * listener's accept queue.  Runs from the bottom half under g_net_lock only, so
 * this is where the allocation and the g_net_lock-protected push belong.
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
    uint32_t tail = __atomic_load_n(&st->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&st->head, __ATOMIC_ACQUIRE);
    bool woke = false;

    while (tail != head) {
        net_accept_stage_slot_t *c = &st->slots[tail & (NET_ACCEPT_STAGE_SIZE - 1)];

#if CONFIG_NET_RACE_DELAY_US
        /* Diagnostic amplifier; see net_profile.h.  Placed in the gap between
         * the dequeue and the g_lwip_lock acquisition below on purpose -- that
         * gap is the window this exists to widen. */
        for (volatile uint32_t d = 0; d < CONFIG_NET_RACE_DELAY_US; d++)
            __asm__ __volatile__("" ::: "memory");
#endif

        net_socket_t *child = NULL;
        if (listener->accept_count >= NET_MAX_QUEUE) {
            /* The application is not accepting.  Refusing here is the same
             * choice the socket-layer connect path makes, and the peer sees a
             * reset rather than a connection nobody will ever accept. */
            a20_perf_count(A20_PERF_NET_ACCEPT_DROP);
        } else if (!(child = net_socket_alloc())) {
            a20_perf_count(A20_PERF_NET_ALLOC_FAIL);
        }
        if (child) {
            child->domain = AF_INET;
            child->type = SOCK_STREAM;
            child->protocol = listener->protocol;
            child->bound = 1;
            child->connected = 1;
            child->ever_connected = 1;
            child->nonblock = listener->nonblock;
            child->tcp_nodelay = listener->tcp_nodelay;
            child->keepalive = listener->keepalive;
            child->keep_idle = listener->keep_idle;
            child->keep_intvl = listener->keep_intvl;
            child->keep_cnt = listener->keep_cnt;
            child->recv_timeout_ticks = listener->recv_timeout_ticks;
            child->send_timeout_ticks = listener->send_timeout_ticks;
            memcpy(child->local, listener->local, listener->local_len);
            child->local_len = listener->local_len;
        }

        /* The listener holds g_net_lock, so a short g_lwip_lock acquisition for
         * the pcb handoff cannot deadlock against the bh_ring producer, which
         * never takes g_net_lock. */
        uint64_t lf = a20_lwip_lock();
        struct tcp_pcb *pcb = c->pcb;
        bool adopted = false;
        bool fin = c->fin;
        bool payload = (c->pending != NULL);
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
             * it.  Copying into the ring is safe under g_lwip_lock: a TCP-sized
             * segment always takes the inline arm, so memp is not touched. */
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
                 * first read() blocks on a close that has already happened. */
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
        if (net_accept_queue_push_locked(listener, child) < 0) {
            net_unregister_socket_locked(child);
            uint64_t cf = a20_lwip_lock();
            tcp_abort(child->tcp);
            child->tcp = NULL;
            a20_lwip_unlock(cf);
            net_socket_free(child);
            a20_perf_count(A20_PERF_NET_ACCEPT_DROP);
            continue;
        }
        if (fin || payload) {
            /* The ring and the EOF flag both landed on the child before it was
             * registered, so nothing would otherwise ask its bottom half to
             * look at them. */
            net_inet_bh_schedule(child);
        }
        a20_perf_count(A20_PERF_NET_ACCEPT_QUEUED);
        net_event_notify(listener, A20_EVENT_ACCEPT_READY, 0, 0);
        if (wait_queue_collect_one(&listener->accept_waitq, 0,
                                   PROC_WAKE_EVENT, wake_q))
            woke = true;
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
 * Runs with g_net_lock held and WITHOUT g_lwip_lock.  Allocates net_msg_t
 * entries, copies staged payload data, and detaches waiters into wake_q.
 * The caller flushes wake_q only after dropping g_net_lock.
 */
#define NET_BH_DRAIN_READ  (1U << 0)
#define NET_BH_DRAIN_WRITE (1U << 1)
#define NET_BH_DRAIN_ACCEPT (1U << 2)

static unsigned
net_inet_bottom_half_process_socket_locked(net_socket_t *s,
                                           proc_wake_q_t *wake_q)
{
    unsigned drain = 0;
    if (s->listening && s->accept_stage.head != s->accept_stage.tail) {
        if (net_inet_accept_stage_drain(s, wake_q))
            drain |= NET_BH_DRAIN_ACCEPT;
    }
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

    if (__atomic_exchange_n(&s->bh_error, 0, __ATOMIC_ACQUIRE)) {
        s->tcp_connecting = 0;
        s->tcp_err = __atomic_load_n(&s->bh_err_code, __ATOMIC_RELAXED);
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

    if (__atomic_exchange_n(&s->bh_closed, 0, __ATOMIC_ACQUIRE)) {
        s->closed = 1;
        net_event_notify(s, A20_EVENT_CLOSED, 0, 0);
        if (net_wait_queue_collect_all_locked(
                &s->read_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_READ;
        if (net_wait_queue_collect_all_locked(
                &s->write_waitq, PROC_WAKE_EVENT, wake_q))
            drain |= NET_BH_DRAIN_WRITE;
    }

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
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (net_socket_is_valid_locked(s) && !s->closed)
        drain = net_inet_bottom_half_process_socket_locked(s, &wake_q);
    __atomic_store_n(&s->bh_pending, 0, __ATOMIC_RELEASE);
    net_bh_slot_clear(s->reg_idx);
    spin_unlock_irqrestore(&g_net_lock, irq);
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
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!__atomic_load_n(&g_net_bh_pending[i], __ATOMIC_ACQUIRE))
            continue;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        unsigned drain = 0;
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        net_socket_t *s = g_sockets[i];
        if (!s || !net_socket_is_valid_locked(s) || s->closed) {
            net_bh_slot_clear(i);
            if (s)
                __atomic_store_n(&s->bh_pending, 0, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&g_net_lock, irq);
            continue;
        }
        drain = net_inet_bottom_half_process_socket_locked(s, &wake_q);
        __atomic_store_n(&s->bh_pending, 0, __ATOMIC_RELEASE);
        net_bh_slot_clear(i);
        spin_unlock_irqrestore(&g_net_lock, irq);
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
    if (s->keepalive)
        pcb->so_options |= SOF_KEEPALIVE;
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
    if (s->domain == AF_INET && s->type == SOCK_STREAM) {
        s->tcp = tcp_new_ip_type(IPADDR_TYPE_V4);
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
            tcp_arg(s->tcp, NULL);
            tcp_recv(s->tcp, NULL);
            tcp_err(s->tcp, NULL);
            tcp_sent(s->tcp, NULL);
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

int net_inet_bind_pcb(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;
    if (s->domain == AF_INET6)
        return 0;
    if (s->udp) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        uint64_t flags = a20_lwip_lock();
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
        err_t e = tcp_bind(s->tcp, &ip, port);
        a20_lwip_unlock(flags);
        return e == ERR_OK ? 0 : -EADDRINUSE;
    }
    return 0;
}

static int net_inet_connect_stream(net_socket_t *s, const void *addr, size_t addrlen,
                                   const void *connect_addr, size_t peer_len)
{
    if (!s->bound) {
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        if (!s->bound)
            net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
        spin_unlock_irqrestore(&g_net_lock, irq);
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
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    net_socket_t *listener =
        local_target ? net_find_stream_listener_locked(s, connect_port) : NULL;
    if (listener && listener->listening && listener->accept_count < NET_MAX_QUEUE) {
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
        child->peer = s;
        s->peer = child;
        s->connected = 1;
        s->ever_connected = 1;
        s->local_tcp = 1;
        child->local_tcp = 1;
        int rr = net_register_socket_locked(child);
        if (rr < 0) {
            s->connected = 0;
            s->peer = NULL;
            net_socket_free(child);
            spin_unlock_irqrestore(&g_net_lock, irq);
            return rr;
        }
        int qr = net_accept_queue_push_locked(listener, child);
        if (qr < 0) {
            s->connected = 0;
            s->peer = NULL;
            net_unregister_socket_locked(child);
            net_socket_free(child);
            spin_unlock_irqrestore(&g_net_lock, irq);
            return qr;
        }
        ktrace_net("[NET] connect: pushed child to listener accept queue\n");
        net_event_notify(listener, A20_EVENT_ACCEPT_READY, 0, 0);
        (void)wait_queue_collect_one(
            &listener->accept_waitq, 0, PROC_WAKE_EVENT, &wake_q);
        spin_unlock_irqrestore(&g_net_lock, irq);
        (void)proc_wake_q_flush(&wake_q);
        net_tcp_drop_pcb(s);
        ktrace_net("[NET] connect: local TCP connect done\n");
        return 0;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);
    net_socket_free(child);

    if (s->domain != AF_INET || !s->tcp) {
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
        uint64_t wait_irq = spin_lock_irqsave(&g_net_lock);
        if (!s->tcp_connecting) {
            spin_unlock_irqrestore(&g_net_lock, wait_irq);
            break;
        }
        if (net_task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, wait_irq);
            net_tcp_drop_pcb(s);
            s->tcp_connecting = 0;
            s->connected = 0;
            return -ERESTARTSYS;
        }
        spin_unlock_irqrestore(&g_net_lock, wait_irq);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        wait_irq = spin_lock_irqsave(&g_net_lock);
        if (!s->tcp_connecting) {
            spin_unlock_irqrestore(&g_net_lock, wait_irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        if (net_task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, wait_irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            net_tcp_drop_pcb(s);
            s->tcp_connecting = 0;
            s->connected = 0;
            return -ERESTARTSYS;
        }
        bool linked = wait_queue_link(&s->read_waitq, &entry, token, 0);
        spin_unlock_irqrestore(&g_net_lock, wait_irq);
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
 * the wire.  Both callers read the fields without g_net_lock, matching the
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
            udp_bind(s->udp, &any, net_ntohs(port));
            a20_lwip_unlock(flags);
        }
    }
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    net_socket_t *local_dst = NULL;
    const void *dst_addr = addr;
    size_t dst_len = addrlen;
    if (!dst_addr && s->connected) {
        dst_addr = s->peer_addr;
        dst_len = s->peer_len;
    }
    if (s->peer && net_socket_is_valid_locked(s->peer)) {
        local_dst = s->peer;
    } else {
        if (s->peer) s->peer = NULL;
        if (dst_addr)
            local_dst = net_find_udp_dst_locked(s, dst_addr, dst_len);
    }
    if (local_dst) {
        int dontwait = s->nonblock || ((flags & MSG_DONTWAIT) != 0);
        if (local_dst->rx_count >= NET_MAX_QUEUE && !dontwait) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return net_enqueue_msg_blocking(s, local_dst, buf, len,
                                            s->local, s->local_len,
                                            dontwait, s->send_timeout_ticks);
        }
        int rr = net_enqueue_msg_locked(local_dst, buf, len, s->local, s->local_len);
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        if (rr >= 0)
            (void)wait_queue_collect_one(
                &local_dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
        spin_unlock_irqrestore(&g_net_lock, irq);
        (void)proc_wake_q_flush(&wake_q);
        return rr;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);

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
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        net_socket_t *dst = s->peer;
        int rv;
        if (!dst || !net_socket_is_valid_locked(dst) || dst->closed) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -ENOTCONN;
        }
        spin_unlock_irqrestore(&g_net_lock, irq);
        rv = net_enqueue_msg_blocking(s, dst, buf, len,
                                      s->local, s->local_len,
                                      s->nonblock, s->send_timeout_ticks);
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
         * g_net_lock, which is never held together with g_lwip_lock.  One
         * drain after the loop covers the same ground.
         */
        uint64_t lwip_flags = a20_lwip_lock();
        a20_lwip_poll_locked();
        int tcp_alive = s->tcp && !s->closed && s->connected;
        u16_t room = tcp_alive ? tcp_sndbuf(s->tcp) : 0;
        if (!tcp_alive) {
            a20_lwip_unlock(lwip_flags);
            return sent ? (int)sent : -EPIPE;
        }
        net_inet_ip_opts_apply_locked(s);
        if (room == 0) {
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
            uint64_t irq = spin_lock_irqsave(&g_net_lock);
            bool linked =
                wait_queue_link(&s->write_waitq, &entry, token, 0);
            spin_unlock_irqrestore(&g_net_lock, irq);
            uint64_t room_flags = a20_lwip_lock();
            int room_now = s->tcp && tcp_sndbuf(s->tcp) > 0;
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
        /* room > 0: merge sndbuf check with write/output under one lock
         * acquisition.  The earlier lock acquired tcp_sndbuf and confirmed
         * liveness; re-verify under the same lock before writing. */
        size_t n = len - sent;
        if (n > room)
            n = room;
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
        tcp_backlog_accepted(s->tcp);
        a20_lwip_unlock(flags);
    }
}

void net_tcp_recved(net_socket_t *s, size_t len) {
    if (s && s->tcp && len > 0) {
        uint64_t flags = a20_lwip_lock();
        while (len > 0) {
            uint16_t n = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
            tcp_recved(s->tcp, n);
            len -= n;
        }
        a20_lwip_unlock(flags);
    }
}
