/*
 * AF_PACKET: raw L2 sockets, which is what udhcpc uses to run DHCP.
 *
 * TX is synchronous: a frame handed to a bound socket goes straight to the
 * device send op.  RX cannot be, because lwip_stack.c captures frames while
 * holding g_lwip_lock and that lock is never held together with a socket-table
 * net lock.  Captures therefore land in a small ring and are delivered from
 * the poll bottom half, which runs with socket locks only.
 */
#include "net/socket_internal.h"
#include "net/socket_side.h"
#include "net/lwip_stack.h"
#include "core/errno.h"
#include "core/string.h"

#include "lwip/netif.h"

/*
 * Both the depth and the slot size follow the profile.  This array is
 * unconditional .bss: 16 slots of 1540 B was 24640 B on every rung, more than
 * a 20 KiB MCU part's entire SRAM, and no runtime counter reports it.  See
 * docs/server-readiness.md and the budget assert in net_profile.h.
 */
#define NET_PACKET_RX_RING   NET_PROFILE_PACKET_RING_SLOTS
#define NET_PACKET_MAX_FRAME NET_PROFILE_PACKET_FRAME_SIZE

typedef struct {
    uint16_t len;
    uint16_t ifindex;
    uint8_t  frame[NET_PACKET_MAX_FRAME];
} net_packet_slot_t;

/*
 * The macro arithmetic in net_profile.h is an upper bound; this is the layout
 * the compiler actually produced, so the budget cannot silently stop
 * describing the array because a field crept into the slot.  The four bytes of
 * len + ifindex are padding-free here only because every rung's frame size is
 * even, which keeps the struct at an even size with no tail padding.
 */
_Static_assert(sizeof(net_packet_slot_t) == NET_PROFILE_PACKET_SLOT_BYTES,
               "net_packet_slot_t no longer matches the profile's per-slot "
               "accounting; update NET_PROFILE_PACKET_SLOT_BYTES with the real "
               "layout rather than letting the static budget become fiction");

static net_packet_slot_t g_pkt_ring[NET_PACKET_RX_RING];
static net_packet_slot_t g_pkt_drain;
static unsigned g_pkt_head;
static unsigned g_pkt_tail;
static spinlock_t g_pkt_ring_lock = SPINLOCK_INIT;
static volatile int g_pkt_pending;
static volatile unsigned g_pkt_drops;

/*
 * Bytes this file places in .bss unconditionally, for /proc/a20/netmem.
 *
 * The pool table above it reports what the lwIP heap hands out, and with
 * MEMP_MEM_MALLOC=1 the heap is exactly where these arrays are *not*: they are
 * reserved whether or not a single frame is ever captured.  Reporting them is
 * what lets a tier's static footprint be read off a running system instead of
 * only off a linker's symbol table, which is how the profile-scope fix for
 * docs/server-readiness.md is meant to be checked in the future.
 */
size_t net_packet_static_bytes(void)
{
    return sizeof(g_pkt_ring) + sizeof(g_pkt_drain);
}

/*
 * Number of AF_PACKET sockets currently holding a bind filter, and the slot
 * bitmap that makes the accounting idempotent.
 *
 * Capturing costs a fixed-size copy of every frame on every interface plus a
 * walk of the whole registry per delivered frame, and on a host that never
 * opened a packet socket all of it is thrown away -- the ring is
 * NET_PACKET_RX_RING frames deep, so a burst overflows it and the frames are
 * dropped having already been copied.  A census turns both of those into a
 * single atomic load.
 *
 * The count is read on the receive path with g_lwip_lock held and is the only
 * lock-free reader, so it is published with a release and sampled with an
 * acquire: a bind that has returned is visible to every later receive.  A
 * receive that raced a close either captured a frame whose socket is now gone
 * -- delivered to nobody, the same outcome as the drop it replaces -- or
 * missed a frame that arrived before the bind completed.  Neither loses a
 * frame the socket was entitled to.
 */
static volatile int g_pkt_bound_count;

int net_packet_bound_count(void)
{
    return __atomic_load_n(&g_pkt_bound_count, __ATOMIC_ACQUIRE);
}

/* Both of these run under the socket's own lock, so the idempotence
 * marker -- now a field of net_socket_t rather than a NET_MAX_SOCKETS-entry
 * bitmap indexed by registry slot -- needs no lock of its own.  It exists to
 * keep the count balanced when the release side cannot tell whether the
 * acquire side ran. */
void net_packet_bound_acquire(net_socket_t *s)
{
    if (!s || s->pkt_bound_marked)
        return;
    s->pkt_bound_marked = 1;
    __atomic_fetch_add(&g_pkt_bound_count, 1, __ATOMIC_ACQ_REL);
}

void net_packet_bound_release(net_socket_t *s)
{
    if (!s || !s->pkt_bound_marked)
        return;
    s->pkt_bound_marked = 0;
    __atomic_fetch_sub(&g_pkt_bound_count, 1, __ATOMIC_ACQ_REL);
}

int net_packet_rx_pending(void)
{
    return __atomic_load_n(&g_pkt_pending, __ATOMIC_ACQUIRE) != 0;
}

void net_packet_rx_defer(unsigned ifindex, const uint8_t *frame, size_t len)
{
    if (!frame || len == 0)
        return;
    if (!net_packet_bound_count())
        return;
    if (len > NET_PACKET_MAX_FRAME)
        len = NET_PACKET_MAX_FRAME;

    uint64_t flags = spin_lock_irqsave(&g_pkt_ring_lock);
    unsigned next = (g_pkt_head + 1) % NET_PACKET_RX_RING;
    if (next == g_pkt_tail) {
        g_pkt_drops++;
        spin_unlock_irqrestore(&g_pkt_ring_lock, flags);
        return;
    }
    net_packet_slot_t *slot = &g_pkt_ring[g_pkt_head];
    slot->len = (uint16_t)len;
    slot->ifindex = (uint16_t)ifindex;
    memcpy(slot->frame, frame, len);
    g_pkt_head = next;
    __atomic_store_n(&g_pkt_pending, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_pkt_ring_lock, flags);
}

/*
 * Deliver one captured frame to every AF_PACKET socket it matches.
 *
 * The table is walked one bucket at a time: the whole set of shard locks with
 * interrupts disabled is the livelock fs/vfs/dcache.c records, and a frame
 * delivery that livelocks the interrupt that is trying to deliver the next
 * frame is the worst possible place to find out.  Each bucket is taken,
 * drained and released before the next is taken, so at most one shard lock is
 * ever held.
 */
static void net_packet_deliver(const net_packet_slot_t *slot,
                               proc_wake_q_t *wake_q)
{
    if (slot->len < ETH_HLEN)
        return;
    /* The ring can still hold frames captured for a socket that has since
     * closed, so the census is re-read here rather than trusted from the
     * capture side. */
    if (!net_packet_bound_count())
        return;
    uint16_t ethertype = (uint16_t)((slot->frame[12] << 8) | slot->frame[13]);

    for (int bucket = 0; bucket < NET_SOCK_BUCKETS; bucket++) {
        uint64_t bf = net_bucket_lock(bucket);
        int base = bucket << NET_SOCK_BUCKET_SHIFT;
        for (int i = 0; i < NET_SOCK_SLOTS_PER_BUCKET; i++) {
        net_socket_t *s = g_sockets[base + i];
        if (!s)
            continue;
        /* Bucket outer, socket inner: the same nesting net_bucket_scan() does,
         * and the only order a bucket lock is ever taken in. */
        uint64_t sf = net_sock_lock(s);
        bool skip = s->domain != AF_PACKET || s->closed || !s->pkt_bound ||
                    (s->pkt_ifindex > 0 &&
                     (unsigned)s->pkt_ifindex != slot->ifindex) ||
                    (s->pkt_protocol != ETH_P_ALL &&
                     s->pkt_protocol != ethertype) ||
                    s->rx_count >= NET_MAX_QUEUE;
        if (!skip) {
            net_sockaddr_ll_t ll;
            memset(&ll, 0, sizeof(ll));
            ll.sll_family = AF_PACKET;
            ll.sll_protocol = net_ntohs(ethertype);
            ll.sll_ifindex = (int32_t)slot->ifindex;
            ll.sll_hatype = ARPHRD_ETHER;
            ll.sll_pkttype = PACKET_HOST;
            ll.sll_halen = ETH_ALEN;
            memcpy(ll.sll_addr, slot->frame + ETH_ALEN, ETH_ALEN);

            const uint8_t *payload = slot->frame;
            size_t payload_len = slot->len;
            if (s->type != SOCK_RAW) {
                payload += ETH_HLEN;
                payload_len -= ETH_HLEN;
            }
            if (net_enqueue_msg_locked(s, payload, payload_len, &ll,
                                       sizeof(ll)) >= 0)
                (void)wait_queue_collect_one(&s->read_waitq, 0,
                                             PROC_WAKE_EVENT, wake_q);
        }
        net_sock_unlock(s, sf);
        }
        net_bucket_unlock(bucket, bf);
    }
}

void net_packet_bottom_half_process(void)
{
    if (!net_packet_rx_pending())
        return;

    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    int delivered = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_pkt_ring_lock);
        if (g_pkt_tail == g_pkt_head) {
            __atomic_store_n(&g_pkt_pending, 0, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&g_pkt_ring_lock, flags);
            break;
        }
        memcpy(&g_pkt_drain, &g_pkt_ring[g_pkt_tail], sizeof(g_pkt_drain));
        g_pkt_tail = (g_pkt_tail + 1) % NET_PACKET_RX_RING;
        spin_unlock_irqrestore(&g_pkt_ring_lock, flags);

        net_packet_deliver(&g_pkt_drain, &wake_q);
        delivered++;
    }
    if (delivered)
        (void)proc_wake_q_flush(&wake_q);
}

int net_packet_ifindex_by_name(const char *name)
{
    if (!name || !name[0])
        return -1;
    u8_t idx = netif_name_to_index(name);
    if (idx == NETIF_NO_INDEX)
        return -1;
    return (int)idx;
}

int net_packet_socket_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s || s->domain != AF_PACKET)
        return -EAFNOSUPPORT;
    if (!addr || addrlen < sizeof(net_sockaddr_ll_t))
        return -EINVAL;

    const net_sockaddr_ll_t *ll = (const net_sockaddr_ll_t *)addr;
    if (ll->sll_family != AF_PACKET)
        return -EAFNOSUPPORT;
    if (ll->sll_ifindex < 0)
        return -EINVAL;
    if (ll->sll_ifindex > 0 && a20_lwip_if_up((unsigned)ll->sll_ifindex) < 0)
        return -ENODEV;

    uint16_t proto = ll->sll_protocol ? net_ntohs(ll->sll_protocol)
                                      : net_ntohs((uint16_t)s->protocol);
    if (proto == 0)
        proto = ETH_P_ALL;

    uint64_t flags = net_sock_lock(s);
    if (!net_socket_is_live(s)) {
        net_sock_unlock(s, flags);
        return -ENOTSOCK;
    }
    s->pkt_ifindex = ll->sll_ifindex;
    s->pkt_protocol = proto;
    s->pkt_hatype = ARPHRD_ETHER;
    s->pkt_halen = ll->sll_halen > sizeof(ll->sll_addr) ? ETH_ALEN
                                                        : ll->sll_halen;
    memcpy(s->pkt_haddr, ll->sll_addr, sizeof(s->pkt_haddr));
    s->pkt_bound = 1;
    /* Published last, under the same lock the release side takes, so a bind
     * that returns is always counted and a rebind cannot double-count. */
    net_packet_bound_acquire(s);
    memcpy(s->local, addr, addrlen);
    s->local_len = addrlen;
    s->bound = 1;
    net_sock_unlock(s, flags);
    return 0;
}

int net_packet_socket_send(net_socket_t *s, const void *buf, size_t len,
                           const void *addr, size_t addrlen)
{
    if (!s || s->domain != AF_PACKET)
        return -EAFNOSUPPORT;
    if (!buf || len == 0)
        return -EINVAL;
    if (!s->pkt_bound)
        return -EDESTADDRREQ;

    const net_sockaddr_ll_t *ll = NULL;
    int ifindex = s->pkt_ifindex;
    if (addr && addrlen >= sizeof(net_sockaddr_ll_t)) {
        ll = (const net_sockaddr_ll_t *)addr;
        if (ll->sll_family != AF_PACKET)
            return -EAFNOSUPPORT;
        if (ll->sll_ifindex > 0)
            ifindex = ll->sll_ifindex;
    }
    if (ifindex <= 0)
        ifindex = a20_lwip_if_default_index();
    if (ifindex <= 0)
        return -ENODEV;

    uint8_t frame[NET_PACKET_MAX_FRAME];
    size_t frame_len;

    if (s->type == SOCK_RAW) {
        if (len > sizeof(frame))
            return -EMSGSIZE;
        memcpy(frame, buf, len);
        frame_len = len;
    } else {
        if (len + ETH_HLEN > sizeof(frame))
            return -EMSGSIZE;

        uint8_t src[8];
        if (a20_lwip_if_hwaddr((unsigned)ifindex, src) < 0)
            return -ENODEV;

        uint8_t dst[ETH_ALEN];
        if (ll && ll->sll_halen == ETH_ALEN)
            memcpy(dst, ll->sll_addr, ETH_ALEN);
        else if (s->pkt_halen == ETH_ALEN)
            memcpy(dst, s->pkt_haddr, ETH_ALEN);
        else
            memset(dst, 0xff, ETH_ALEN);

        uint16_t proto = s->pkt_protocol;
        if (ll && ll->sll_protocol)
            proto = net_ntohs(ll->sll_protocol);

        memcpy(frame, dst, ETH_ALEN);
        memcpy(frame + ETH_ALEN, src, ETH_ALEN);
        frame[12] = (uint8_t)(proto >> 8);
        frame[13] = (uint8_t)(proto & 0xff);
        memcpy(frame + ETH_HLEN, buf, len);
        frame_len = len + ETH_HLEN;
    }

    if (a20_lwip_packet_tx((unsigned)ifindex, frame, frame_len) < 0)
        return -EIO;
    return (int)len;
}
