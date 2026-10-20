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
#include <stddef.h>

#include "lwip/netif.h"

/*
 * Both the depth and the slot size follow the profile.  This array is
 * unconditional .bss: 16 slots of 1540 B was 24640 B on every rung, more than
 * a 20 KiB MCU part's entire SRAM, and no runtime counter reports it.  See
 * docs/status/current/server-readiness.md and the budget assert in net_profile.h.
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

#include "net/socket_packet_state.h"

/*
 * The AF_PACKET state machine is Rust (kernel/net/socket_packet.rs).  What is
 * left here is the ring's storage and its spinlock, the primitive set the Rust
 * module calls, and the wrappers that keep the C names the rest of the tree
 * uses.
 *
 * Rust owns the capture gate and the ring protocol (full/empty, the drop
 * counter, the pending publication, the drain loop and the flush decision), the
 * delivery filter and the ancillary address, the bound-socket census, and the
 * bind and send transactions.  The storage stays here so the profile's static
 * accounting (net_packet_static_bytes(), reported on /proc/a20/netmem) keeps
 * describing the real layout, and the spinlock stays here because it is the
 * primitive the ring protocol is built on.
 */

static net_packet_slot_t g_pkt_ring[NET_PACKET_RX_RING];
static net_packet_slot_t g_pkt_drain;
static unsigned g_pkt_head;
static unsigned g_pkt_tail;
static spinlock_t g_pkt_ring_lock = SPINLOCK_INIT;
static volatile int g_pkt_pending;
static volatile unsigned g_pkt_drops;

/*
 * The slot layout the Rust side reads: `len` at 0, `ifindex` at 2, the frame at
 * 4.  The profile's slot accounting already pins the total; these pin the
 * offsets the Rust view assumes, so a field creeping into the slot fails the
 * build instead of silently reinterpreting the bytes.
 */
_Static_assert(offsetof(net_packet_slot_t, len) == 0 &&
               offsetof(net_packet_slot_t, ifindex) == 2 &&
               offsetof(net_packet_slot_t, frame) == 4,
               "kernel/net/socket_packet.rs reads the slot at these offsets");
/* The Rust frame buffer is 1536 bytes (the largest rung's frame size). */
_Static_assert(NET_PACKET_MAX_FRAME <= 1536,
               "the Rust packet frame buffer must cover the profile's frame");
/* net_sockaddr_ll_t as kernel/net/socket_packet.rs declares it. */
_Static_assert(sizeof(net_sockaddr_ll_t) == 20 &&
               offsetof(net_sockaddr_ll_t, sll_halen) == 11 &&
               offsetof(net_sockaddr_ll_t, sll_addr) == 12,
               "kernel/net/socket_packet.rs declares net_sockaddr_ll_t at this layout");

size_t net_packet_static_bytes(void)
{
    return sizeof(g_pkt_ring) + sizeof(g_pkt_drain);
}

void a20_net_packet_ring_init(net_packet_ring_t *out)
{
    out->head = &g_pkt_head;
    out->tail = &g_pkt_tail;
    out->pending = &g_pkt_pending;
    out->drops = &g_pkt_drops;
    out->slots = (uint8_t *)g_pkt_ring;
    out->drain = (uint8_t *)&g_pkt_drain;
    out->depth = NET_PACKET_RX_RING;
    out->slot_bytes = sizeof(net_packet_slot_t);
    out->max_queue = NET_MAX_QUEUE;
}

void a20_net_packet_ring_lock(uint64_t *flags)
{
    *flags = spin_lock_irqsave(&g_pkt_ring_lock);
}

void a20_net_packet_ring_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&g_pkt_ring_lock, flags);
}

void a20_net_packet_view_init(void *sp, net_packet_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->protocol = &s->protocol;
    out->closed = &s->closed;
    out->rx_count = &s->rx_count;
    out->bound = &s->bound;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->pkt_ifindex = &s->pkt_ifindex;
    out->pkt_protocol = &s->pkt_protocol;
    out->pkt_hatype = &s->pkt_hatype;
    out->pkt_halen = &s->pkt_halen;
    out->pkt_haddr = s->pkt_haddr;
    out->pkt_bound = &s->pkt_bound;
    out->pkt_bound_marked = &s->pkt_bound_marked;
}

int a20_net_packet_ns_initial(void *ns)
{
    return resource_ns_is_initial((const resource_namespace_t *)ns) ? 1 : 0;
}

int a20_net_packet_buckets(void)
{
    return NET_SOCK_BUCKETS;
}

int a20_net_packet_slots_per_bucket(void)
{
    return NET_SOCK_SLOTS_PER_BUCKET;
}

void a20_net_packet_bucket_lock(int bucket, uint64_t *flags)
{
    *flags = net_bucket_lock(bucket);
}

void a20_net_packet_bucket_unlock(int bucket, uint64_t flags)
{
    net_bucket_unlock(bucket, flags);
}

void *a20_net_packet_slot_socket(int bucket, int i)
{
    return g_sockets[(bucket << NET_SOCK_BUCKET_SHIFT) + i];
}

void a20_net_packet_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_packet_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

int a20_net_packet_is_live(void *s)
{
    return net_socket_is_live((const net_socket_t *)s) ? 1 : 0;
}

int a20_net_packet_enqueue(void *s, const uint8_t *payload, size_t len,
                           const void *ll, size_t lllen)
{
    return net_enqueue_msg_locked((net_socket_t *)s, payload, len, ll, lllen);
}

int a20_net_packet_collect_read(void *s, void *wake_q)
{
    return (int)wait_queue_collect_one(&((net_socket_t *)s)->read_waitq, 0,
                                       PROC_WAKE_EVENT,
                                       (proc_wake_q_t *)wake_q);
}

void a20_net_packet_wakeq_init(void *wake_q)
{
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void a20_net_packet_wakeq_flush(void *wake_q)
{
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

int a20_net_packet_if_up(int ifindex)
{
    return a20_lwip_if_up((unsigned)ifindex);
}

int a20_net_packet_if_default_index(void)
{
    return a20_lwip_if_default_index();
}

int a20_net_packet_if_hwaddr(int ifindex, uint8_t out[8])
{
    return a20_lwip_if_hwaddr((unsigned)ifindex, out);
}

int a20_net_packet_tx(int ifindex, const uint8_t *frame, size_t len)
{
    return a20_lwip_packet_tx((unsigned)ifindex, frame, len);
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

void net_packet_rx_defer(unsigned ifindex, const uint8_t *frame, size_t len)
{
    net_packet_ring_t r;
    a20_net_packet_ring_init(&r);
    a20_net_packet_rx_defer(&r, ifindex, frame, len);
}

void net_packet_bottom_half_process(void)
{
    net_packet_ring_t r;
    a20_net_packet_ring_init(&r);
    a20_net_packet_bottom_half_process(&r);
}

int net_packet_rx_pending(void)
{
    net_packet_ring_t r;
    a20_net_packet_ring_init(&r);
    return a20_net_packet_rx_pending(&r);
}

int net_packet_bound_count(void)
{
    return a20_net_packet_bound_count();
}

void net_packet_bound_acquire(net_socket_t *s)
{
    net_packet_view_t v;
    a20_net_packet_view_init(s, &v);
    a20_net_packet_bound_acquire(&v);
}

void net_packet_bound_release(net_socket_t *s)
{
    net_packet_view_t v;
    a20_net_packet_view_init(s, &v);
    a20_net_packet_bound_release(&v);
}

int net_packet_socket_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    return a20_net_packet_socket_bind(s, addr, addrlen);
}

int net_packet_socket_send(net_socket_t *s, const void *buf, size_t len,
                           const void *addr, size_t addrlen)
{
    return a20_net_packet_socket_send(s, buf, len, addr, addrlen,
                                      NET_PACKET_MAX_FRAME);
}
