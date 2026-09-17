/*
 * AF_PACKET: raw L2 sockets, which is what udhcpc uses to run DHCP.
 *
 * TX is synchronous: a frame handed to a bound socket goes straight to the
 * device send op.  RX cannot be, because lwip_stack.c captures frames while
 * holding g_lwip_lock and that lock is never held together with g_net_lock.
 * Captures therefore land in a small ring and are delivered from the poll
 * bottom half, which runs with g_net_lock only.
 */
#include "net/socket_internal.h"
#include "net/lwip_stack.h"
#include "core/errno.h"
#include "core/string.h"

#include "lwip/netif.h"

#define NET_PACKET_RX_RING   16
#define NET_PACKET_MAX_FRAME 1536

typedef struct {
    uint16_t len;
    uint16_t ifindex;
    uint8_t  frame[NET_PACKET_MAX_FRAME];
} net_packet_slot_t;

static net_packet_slot_t g_pkt_ring[NET_PACKET_RX_RING];
static net_packet_slot_t g_pkt_drain;
static unsigned g_pkt_head;
static unsigned g_pkt_tail;
static spinlock_t g_pkt_ring_lock = SPINLOCK_INIT;
static volatile int g_pkt_pending;
static volatile unsigned g_pkt_drops;

int net_packet_rx_pending(void)
{
    return __atomic_load_n(&g_pkt_pending, __ATOMIC_ACQUIRE) != 0;
}

void net_packet_rx_defer(unsigned ifindex, const uint8_t *frame, size_t len)
{
    if (!frame || len == 0)
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

static void net_packet_deliver_locked(const net_packet_slot_t *slot,
                                      proc_wake_q_t *wake_q)
{
    if (slot->len < ETH_HLEN)
        return;
    uint16_t ethertype = (uint16_t)((slot->frame[12] << 8) | slot->frame[13]);

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = g_sockets[i];
        if (!s || s->domain != AF_PACKET || s->closed || !s->pkt_bound)
            continue;
        if (s->pkt_ifindex > 0 && (unsigned)s->pkt_ifindex != slot->ifindex)
            continue;
        if (s->pkt_protocol != ETH_P_ALL && s->pkt_protocol != ethertype)
            continue;
        if (s->rx_count >= NET_MAX_QUEUE)
            continue;

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
        if (net_enqueue_msg_locked(s, payload, payload_len, &ll, sizeof(ll)) >= 0)
            (void)wait_queue_collect_one(&s->read_waitq, 0,
                                         PROC_WAKE_EVENT, wake_q);
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

        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        net_packet_deliver_locked(&g_pkt_drain, &wake_q);
        spin_unlock_irqrestore(&g_net_lock, irq);
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

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    s->pkt_ifindex = ll->sll_ifindex;
    s->pkt_protocol = proto;
    s->pkt_hatype = ARPHRD_ETHER;
    s->pkt_halen = ll->sll_halen > sizeof(ll->sll_addr) ? ETH_ALEN
                                                        : ll->sll_halen;
    memcpy(s->pkt_haddr, ll->sll_addr, sizeof(s->pkt_haddr));
    s->pkt_bound = 1;
    memcpy(s->local, addr, addrlen);
    s->local_len = addrlen;
    s->bound = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
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
