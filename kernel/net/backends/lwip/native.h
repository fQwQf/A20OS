/* Private adapter contract. Nothing here is included by the socket ABI or
 * control plane. The pristine stack has one serialized core, no PCB lanes. */
#ifndef A20_LWIP_NATIVE_H
#define A20_LWIP_NATIVE_H
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#ifdef CONFIG_LWIP_UPSTREAM
#define NET_PCB_LANE_OWNER_OF_PCB(pcb) ((void)(pcb), 0u)
#define NET_PCB_LANE_OF(ip, port) ((void)(ip), (void)(port), 0u)
#define NET_PCB_LANE_OF_PCB(pcb) ((void)(pcb), 0u)
#define NET_PCB_LANE_BUCKETS 1
#define TCP_CONG_RENO 0
#define TCP_CONG_CUBIC 1
#else
#include "lwip/priv/pcb_lane.h"
#include "lwip/priv/tcp_cubic_priv.h"
#endif

#include "net/socket/socket_internal.h"
#include "net/net_lane.h"
typedef enum {
    NET_BH_RECV = 0,
    NET_BH_CONNECTED,
    NET_BH_CLOSED,
    NET_BH_ERROR,
} net_bh_event_type_t;

typedef struct net_bh_event {
    net_bh_event_type_t type;
    int err;
    size_t len;
    uint8_t addr[NET_SOCKADDR_MAX];
    size_t addrlen;
    uint8_t has_pktinfo;
    uint8_t has_hoplimit;
    uint8_t has_tclass;
    uint8_t pktinfo_family;
    uint32_t pktinfo_ifindex;
    union {
        uint8_t pktinfo_addr[16]; /* Existing IPv6/copy view; no layout change. */
        net_pktinfo_address_t pktinfo;
    };
    uint8_t hoplimit;
    uint8_t tclass;
    uint16_t __pad_meta;
    /*
     * Set instead of using `data` when the payload exceeds
     * NET_BH_INLINE_PAYLOAD: a large datagram or lwIP's coalesced TCP chain.
     * `spill` is a pbuf the ring has taken a
     * reference to and `spill_off` is where this event's slice starts in it.
     * The ring frees consumed references under its producer's lane lock, so
     * the pbuf is never released outside the lwIP critical section -- memp has
     * no internal locking and today every memp call happens under
     * g_lwip_lock.
     */
    struct pbuf *spill;
    uint32_t spill_off;
    uint8_t data[NET_BH_INLINE_PAYLOAD];
} net_bh_event_t;

typedef struct net_bh_ring {
    net_bh_event_t events[NET_BH_RING_SIZE];
    /*
     * Spill references owned per slot, indexed the same way as `events`.
     * The bottom-half tail reclaims consumed slots after releasing the socket
     * lock; producer reuse and socket teardown also drain their references.
     */
    struct pbuf *owned[NET_BH_RING_SIZE];
    uint32_t head;
    uint32_t tail;
} net_bh_ring_t;

/*
 * One staged inbound connection.
 *
 * The accept callback cannot finish the handoff, so it parks the pcb here with
 * the socket layer's own callbacks already installed on it (see
 * net_inet_tcp_stage_* in socket_inet.c).  Until the bottom half adopts the
 * pcb, that context -- not the listener's net_socket_t -- is what those
 * callbacks see, and it has to carry everything they may observe in the
 * meantime.
 *
 * `pcb` is the identity check.  lwIP can still destroy a pcb behind our back
 * (an RST abandons it outright), so a staged pcb is not guaranteed to survive
 * until adoption; when it does not, `dead` is set and `pcb` is stale from
 * then on and must never be dereferenced.
 *
 * The slot address is what identifies the entry, so it may not be recycled
 * while a pcb can still be pointing at it.  The bottom half therefore hands the
 * pcb over to its child -- re-arging it to the child socket and clearing `pcb`
 * -- before it advances tail past this slot.
 */
typedef struct net_accept_stage_slot {
    struct net_socket *listener;
    struct tcp_pcb *pcb;
    /* Payload the peer sent before the handshake was adopted.  Held by
     * reference: pbuf_free() is only safe under g_lwip_lock, so these are
     * released either in the callback that discovers the pcb died or in the
     * bottom half's g_lwip_lock section. */
    struct pbuf *pending;
    /* Peer sent a FIN while staged.  The connection is still ours to hand to
     * accept(), so this is recorded and replayed onto the child instead of
     * being closed out from under it. */
    uint32_t fin;
    uint32_t dead;
    int error;
} net_accept_stage_slot_t;

/*
 * Accepted-pcb staging for a real lwIP listening socket.
 *
 * lwIP hands a completed handshake to tcp_accept() with g_lwip_lock already
 * held and the new pcb in ESTABLISHED.  The bookkeeping that has to follow --
 * allocating a net_socket_t, installing its callbacks, registering it, pushing
 * it on the listener's accept queue and waking accept_waitq -- needs
 * a socket lock and the allocator, and
 * docs/net/network-lock-contract.md forbids
 * both inside an lwIP callback.  So the callback only parks the pcb here and
 * the bottom half finishes the job, which is the same split the receive path
 * already uses.
 *
 * Bounded and lock-free in the shape of net_bh_ring_t: the producer runs with
 * local interrupts off under g_lwip_lock and publishes head with a release
 * fence, the consumer runs later under the listener's own lock.  `dropped`
 * is a correctness counter, not a statistic -- a non-zero value means a completed handshake was
 * discarded, and the pcb has to be aborted rather than leaked.
 */
#define NET_ACCEPT_STAGE_SIZE 8

typedef struct net_accept_stage {
    net_accept_stage_slot_t slots[NET_ACCEPT_STAGE_SIZE];
    uint32_t head;
    uint32_t tail;
    volatile int dropped;
    /*
     * Single-consumer guard for the drain, guarded by the listener's own lock.
     * The ring has exactly one consumer because a slot's ownership hands over
     * when tail moves, and two drains reading the same tail would both release
     * the same slot.  The single g_net_lock used to provide that exclusion
     * implicitly; the drain now has to drop the listener's lock to register the
     * children (which takes a bucket of its own), so the exclusion is explicit
     * here and the flag is what stands in for the lock in that window.
     */
    int drain_active;
} net_accept_stage_t;


typedef struct lwip_socket {
    struct udp_pcb *udp;
    struct raw_pcb *raw;
    struct tcp_pcb *tcp;
    unsigned lane;
    net_bh_ring_t bh_ring;
    net_accept_stage_t accept_stage;
} lwip_socket_t;
static inline lwip_socket_t *lw(const net_socket_t *s) {
    return (lwip_socket_t *)s->transport_private;
}
void a20_net_config_sync_from_lwip(void);
int net_inet_state_attach(net_socket_t *s);
_Static_assert(sizeof(lwip_socket_t) + sizeof(net_socket_t) <= NET_PROFILE_SOCKET_MAX_BYTES,
               "frontend and private state exceed the per-socket budget");
#if CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_EMBEDDED
_Static_assert(NET_MAX_SOCKETS * (sizeof(lwip_socket_t) + sizeof(net_socket_t))
               <= NET_PROFILE_SOCKET_BUDGET, "embedded transport state exceeds budget");
_Static_assert(NET_MAX_SOCKETS * (sizeof(lwip_socket_t) + sizeof(net_socket_t)) +
                   (NET_PROFILE_PACKET_RING_SLOTS + 1) * NET_PROFILE_PACKET_SLOT_BYTES +
                   NET_PROFILE_NETIF_MAX_DEVS * NET_PROFILE_NETIF_STATE_BYTES +
                   NET_PROFILE_NETIF_STRUCT_BUDGET + NET_PROFILE_FILTER_BUDGET +
                   NET_PROFILE_MEM_SIZE + NET_PROFILE_MEMP_LANE_TABLE_BYTES
               <= NET_PROFILE_TOTAL_BUDGET, "embedded whole-network budget exceeded");
#endif
/* lw(s)->lane is published while bind/PCB rebucket operations may race with
 * packet-side snapshots, so all post-publication accesses use atomics. */
static inline unsigned net_socket_lane_load(const net_socket_t *s)
{
    return lw(s) ? __atomic_load_n(&lw(s)->lane, __ATOMIC_ACQUIRE) : 0;
}

static inline void net_socket_lane_store(net_socket_t *s, unsigned lane)
{
    __atomic_store_n(&lw(s)->lane, lane, __ATOMIC_RELEASE);
}

int      net_sockaddr_to_lwip_ip(const void *addr, size_t len,
                                 ip_addr_t *ip, uint16_t *port);
unsigned net_socket_lane_of_addr(const void *addr, size_t len,
                                 unsigned fallback);
int      net_lwip_ip_to_sockaddr(const ip_addr_t *ip, uint16_t port,
                                 uint8_t out[NET_SOCKADDR_MAX],
                                 size_t *outlen);
void     a20_net_cong_apply(struct tcp_pcb *pcb, uint8_t alg);
void     net_inet_tcp_buf_apply(uint32_t snd_buf, uint32_t rcv_buf,
                               struct tcp_pcb *pcb);
#endif
