#ifndef _NET_SOCKET_INTERNAL_H
#define _NET_SOCKET_INTERNAL_H

#include "net/socket.h"
#include "ipc/ipc.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/timer.h"
#include "lwip/ip_addr.h"
#include "net/net_lane.h"

struct udp_pcb;
struct raw_pcb;
struct tcp_pcb;
struct pbuf;

#include "net/net_profile.h"

#define NET_MAX_SOCKETS NET_PROFILE_MAX_SOCKETS
#define NET_MAX_STREAM_PAYLOAD 2048
#define NET_MAX_PAYLOAD 65535
#define NET_MAX_QUEUE   NET_PROFILE_MAX_QUEUE
#define NET_SCM_MAX_FDS 16
#define NET_CONNECT_TIMEOUT_TICKS MS_TO_TICKS(10000)
#define NET_BH_RING_SIZE NET_PROFILE_BH_RING_SIZE

/*
 * Default SO_SNDBUF / SO_RCVBUF for a datagram socket (UDP, RAW), which have no
 * pcb for either option to mean anything about.
 *
 * Send: a datagram is handed straight to udp_sendto()/raw_send() and the pbuf is
 * freed on the way out, so this socket queues nothing on the send side.  What the
 * ceiling can honestly bound is the single datagram it is willing to hand to the
 * stack, and that is NET_MAX_PAYLOAD -- the largest one the socket layer will
 * stage at all.  Larger requests are clamped to it, and net_inet_send_udp() /
 * net_inet_send_raw() return -EMSGSIZE for a datagram above it.
 *
 * Receive: the socket's receive queue is already capped at NET_MAX_QUEUE messages
 * of at most NET_MAX_PAYLOAD bytes, so that product is the largest byte ceiling
 * the queue can ever reach.  The default is set to exactly that, which is what
 * makes "never set it" behave identically to before this option was accepted for
 * datagrams: the byte check can only fire for a socket that asked for something
 * smaller.  Anything lower and a default UDP socket would start dropping
 * datagrams it used to queue.
 *
 * Neither is Linux's sk_sndbuf / sk_rcvbuf.  Linux bounds unsent skbs and does
 * memory accounting; this bounds one datagram and one queued byte total.  There is
 * no window-scale folding either -- see the doc note in
 * docs/net/network-config-design.md.
 */
#define NET_DGRAM_SND_BUF_DEFAULT ((uint32_t)NET_MAX_PAYLOAD)
#define NET_DGRAM_RCV_BUF_DEFAULT                                              \
    ((uint32_t)NET_MAX_QUEUE * (uint32_t)NET_MAX_PAYLOAD)

/*
 * Payload staging sizes.
 *
 * Both stages below used to embed data[NET_MAX_PAYLOAD] (65535 bytes).  A
 * 1460-byte segment therefore cost ~136 KiB of memset plus three copies, and
 * the memsets ran inside the lwIP critical section -- the source of the
 * single-acquire spin spikes recorded in docs/server-readiness.md, and the
 * reason removing read-path polling did not lower the spin total.  net_msg_t
 * was additionally oversize past SLAB_MAX_OBJ, so mm/slab.c rounded every
 * message up to whole pages: a 100-byte datagram cost 68 KiB.
 *
 * A TCP segment is at most TCP_MSS and most datagrams fit inline, so only
 * oversized payloads take the spill path.  The inline sizes must stay >= the
 * configured TCP_MSS and <= the largest slab class respectively.
 */
#define NET_BH_INLINE_PAYLOAD  NET_PROFILE_INLINE_PAYLOAD
/* Power of two: bh_ring_mask() relies on it. */
_Static_assert((NET_BH_RING_SIZE & (NET_BH_RING_SIZE - 1)) == 0,
               "the bottom-half ring size must be a power of two");
#define NET_MSG_INLINE_PAYLOAD 1024

typedef enum {
    NET_BH_RECV = 0,
    NET_BH_CONNECTED,
    NET_BH_CLOSED,
    NET_BH_ERROR,
} net_bh_event_type_t;

typedef struct net_bh_event {
    struct net_bh_event *next;
    net_bh_event_type_t type;
    int err;
    size_t len;
    uint8_t addr[NET_SOCKADDR_MAX];
    size_t addrlen;
    uint8_t has_pktinfo;
    uint8_t has_hoplimit;
    uint8_t has_tclass;
    uint8_t pad;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    uint8_t hoplimit;
    uint8_t tclass;
    uint16_t __pad_meta;
    /*
     * Set instead of using `data` when the payload exceeds
     * NET_BH_INLINE_PAYLOAD, which only a datagram socket can produce: a TCP
     * segment is at most TCP_MSS.  `spill` is a pbuf the ring has taken a
     * reference to and `spill_off` is where this event's slice starts in it.
     * The ring frees the reference when it next wraps onto the same slot, so
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
     * bh_ring_prepare() reclaims the slot's reference before handing it out
     * again, and socket teardown drains the whole array.
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

typedef struct net_msg {
    struct net_msg *next;
    size_t len;
    size_t off;
    uint8_t addr[NET_SOCKADDR_MAX];
    size_t addrlen;
    uint8_t has_pktinfo;
    uint8_t has_hoplimit;
    uint8_t has_tclass;
    uint8_t pad;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    uint8_t hoplimit;
    uint8_t tclass;
    uint16_t __pad_meta;
    /*
     * SCM_RIGHTS in-flight fd references (vfile_get refs, dup semantics).
     * Ownership transfers to the receiver on the first dequeue of this
     * message; net_msg_free drops whatever is still attached.
     */
    int scm_nfiles;
    vfile_t *scm_files[NET_SCM_MAX_FDS];
    /* SCM_CREDENTIALS: sender pid/uid/gid captured at enqueue time. */
    uint8_t has_cred;
    uint8_t __pad_cred[3];
    int32_t cred_pid;
    int32_t cred_uid;
    int32_t cred_gid;
    /*
     * Payload lives inline up to NET_MSG_INLINE_PAYLOAD, which is sized to land
     * in mm/slab.c's largest slab class so ordinary messages never reach the
     * buddy allocator.  `overflow` is allocated only for a larger payload and
     * is owned exclusively by this message.
     */
    uint8_t inline_data[NET_MSG_INLINE_PAYLOAD];
    uint8_t *overflow;
} net_msg_t;

/*
 * Byte offset 0 of a message's payload, valid for any 0 <= off < m->len.  Both
 * arms are contiguous, so the partial-read path can index either with m->off.
 */
static inline uint8_t *net_msg_payload(net_msg_t *m)
{
    return m->overflow ? m->overflow : m->inline_data;
}

typedef struct net_recv_meta {
    int scm_nfiles;
    vfile_t *scm_files[NET_SCM_MAX_FDS];
    uint8_t has_pktinfo;
    uint8_t has_hoplimit;
    uint8_t has_tclass;
    uint8_t pad;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    uint8_t hoplimit;
    uint8_t tclass;
    uint16_t __pad_meta;
    /* SCM_CREDENTIALS captured from the dequeued message. */
    uint8_t has_cred;
    uint8_t __pad_cred[3];
    int32_t cred_pid;
    int32_t cred_uid;
    int32_t cred_gid;
} net_recv_meta_t;

typedef struct net_socket {
    /*
     * Lane this socket's PCB belongs to, and which never changes for the life of
     * the connection.  Recomputed at bind from the bound (ip, port) because that
     * is the pair an inbound packet can reproduce: the packet's destination
     * address and port are this connection's local address and port.  Before bind
     * it is only a provisional assignment from the creating CPU, which is why
     * nothing may rely on it until the socket is bound -- and the accept path,
     * which never binds, overwrites it with the adopted pcb's owning lane
     * instead (net_inet_accept_stage_drain).
     *
     * The value is always a real lane in 0..CONFIG_NET_LANES-1, wildcard binds
     * included: for bind(0.0.0.0) it is net_lane_of(0, port), which is what lwIP
     * calls the owning lane of the matching wildcard pcb.  It is NOT the same
     * index as that pcb's *bucket*, which for a wildcard bind is the sentinel;
     * see NET_PCB_LANE_OWNER_OF_PCB() and NET_PCB_LANE_OF_PCB() in
     * lwip/priv/pcb_lane.h.
     */
    unsigned lane;
    int domain;
    int type;
    int protocol;
    int nonblock;
    int closed;
    int shut_rd;
    int shut_wr;
    int peer_closed;
    int bound;
    int connected;
    int ever_connected;
    int listening;
    uint8_t local[NET_SOCKADDR_MAX];
    size_t local_len;
    uint8_t peer_addr[NET_SOCKADDR_MAX];
    size_t peer_len;
    struct net_socket *peer;
    /* AF_UNIX socketpair channel-backed data plane (internal IPC bridge):
     * plain data flows through the channel; SCM_RIGHTS messages fall back
     * to the legacy queue (rx_head).  ch_buf holds a stream leftover. */
    a20_channel_ep_t *ch_ep;
    char             *ch_buf;
    uint32_t          ch_len;
    net_msg_t *rx_head;
    net_msg_t *rx_tail;
    int rx_count;
    wait_queue_t accept_waitq;
    wait_queue_t read_waitq;
    wait_queue_t write_waitq;
    struct udp_pcb *udp;
    struct raw_pcb *raw;
    struct tcp_pcb *tcp;
    int local_tcp;
    int tcp_connecting;
    int tcp_err;
    int tcp_nodelay;
    /* Congestion control requested via TCP_CONGESTION, as a TCP_CONG_*
     * value from lwip/priv/tcp_cubic_priv.h.  TCP_CONG_RENO is the default
     * and is also what getsockopt reports for a socket that never set it. */
    uint8_t tcp_congestion;
    /* SO_SNDBUF / SO_RCVBUF, in bytes, as the caller asked for them.
     *
     * These are NOT Linux's sk_sndbuf / sk_rcvbuf and must not be described as
     * such; see the block comment above net_inet_tcp_buf_apply() for exactly
     * what each one does and does not bound.  Zero means "never set", which is
     * why net_socket_alloc() fills in the stack defaults. */
    uint32_t snd_buf;
    uint32_t rcv_buf;
    int reuseaddr;
    int reuseport;
    int ipv6_v6only;
    int keepalive;
    int keep_idle;
    int keep_intvl;
    int keep_cnt;
    /* Per-socket IPPROTO_IP options.  The *_set flags separate "caller asked
     * for 0" from "never asked": TTL 0 and TOS 0 are both legal. */
    uint8_t ip_ttl;
    uint8_t ip_tos;
    uint8_t ip_ttl_set;
    uint8_t ip_tos_set;
    uint8_t mc_ttl;   /* hop count, not a TTL byte; 0 means 1, as Linux does */
    uint8_t mc_loop;
    uint64_t recv_timeout_ticks;
    uint64_t send_timeout_ticks;
    int ipv6_checksum_offset;
    uint32_t icmp6_filter[8];
    int icmp6_filter_set;
    int ipv6_recv_pktinfo;
    int ipv6_recv_tclass;
    int ipv6_recv_hoplimit;
    int ipv6_recv_rthdr;
    int ipv6_recv_hopopts;
    int ipv6_recv_dstopts;
    int ipv6_recv_err;
    int ipv6_recv_2292_pktinfo;
    int ipv6_recv_2292_hoplimit;
    int ipv6_recv_2292_rthdr;
    int ipv6_recv_2292_hopopts;
    int ipv6_recv_2292_dstopts;
    int passcred;   /* SO_PASSCRED: deliver SCM_CREDENTIALS on recvmsg */
    int32_t peer_pid;   /* SO_PEERCRED: peer pid captured on connect/accept */
    int32_t peer_uid;
    int32_t peer_gid;
    int32_t owner_pid;  /* binder creds: what a connecting peer reports */
    int32_t owner_uid;
    int32_t owner_gid;
    /* Channel-backed AF_UNIX: latest sender credentials for SCM_CREDENTIALS
     * when the data plane bypasses the legacy net_msg queue. */
    int32_t ch_cred_pid;
    int32_t ch_cred_uid;
    int32_t ch_cred_gid;
    /* AF_ALG algorithm names from bind().  No kernel crypto provider ships,
     * so bind() always fails and these stay diagnostic-only.  Do not attach
     * a data plane or an in-socket buffer here without a real provider —
     * see kernel/net/socket_alg.c. */
    char alg_type[16];
    char alg_name[64];
    struct net_socket *accept_next;
    struct net_socket *accept_head;
    struct net_socket *accept_tail;
    int accept_count;
    net_accept_stage_t accept_stage;
    /* AF_PACKET: bound L2 filter.  pkt_protocol is host order. */
    int pkt_ifindex;
    uint16_t pkt_protocol;
    uint16_t pkt_hatype;
    uint8_t pkt_halen;
    uint8_t pkt_haddr[8];
    int pkt_bound;
    /* AF_PACKET bind marker.  Used to be one bit of a NET_MAX_SOCKETS-entry
     * bitmap indexed by registry slot (socket_packet.c's g_pkt_bound_slots),
     * which made a single global lock the only thing keeping the accounting
     * balanced.  It is per-socket state, so it lives with the socket and is
     * written under that socket's own lock; the global counter it feeds,
     * g_pkt_bound_count, stays atomic because the lwIP receive path reads it
     * with g_lwip_lock held and never a net lock. */
    int pkt_bound_marked;
    int in_registry;
    int reg_idx;
    /* Reference count, see net_socket_ref() in this header. */
    volatile int refs;
    /*
     * Canary for the reference count, set by net_socket_alloc() and cleared by
     * the one free that actually releases the object.
     *
     * The count alone cannot see a double free: the second net_socket_free()
     * reads a refs value the first one already drove to zero, and on an object
     * that has been handed back to obj_cache that read is whatever the slot now
     * holds.  The canary makes the common case loud instead.  Stated honestly,
     * it is a canary and not a proof: obj_cache_alloc_zero() hands the same
     * address back to the next socket, which sets the canary again, so a double
     * free that lands on a *reused* slot stays invisible.  It catches the case
     * that actually shows up -- a free arriving after the slot was reused by
     * something that is not a socket, or before any reuse at all.
     */
    uint32_t      ref_magic;
    /*
     * Running receive-queue byte/message tally, formerly g_rxq_tally indexed by
     * registry slot -- 512 KiB of table on the server profile for a per-socket
     * number.  High 32 bits are the message count, low 32 the readable bytes;
     * see socket_queue.c for why the pair, not just the byte half, is what
     * makes it safe to trust.  Touched only under this socket's own lock.
     */
    uint64_t rxq_tally;
    int gfd;                       /* creating task's fd carrying this socket */
    struct vfile *vf;              /* the socket's vfile (EventQ identity) */
    /*
     * This socket's own lock: everything above in this struct except the
     * registry slot is protected by it, and by nothing else.  Stage E of
     * docs/net/net-lanes.md moved it off the socket-table bucket lock, which
     * used to serialise every socket against every other socket in the same
     * run of registry slots.  The bucket locks now cover only the slot table
     * itself; see the lock rules below.
     *
     * It is embedded rather than pointed at so that a socket that owns no
     * registry slot -- being created, the accepted end of an AF_UNIX stream --
     * has a working lock anyway.  That case used to need the orphan shard,
     * which no longer exists.
     */
    spinlock_t lock;
    net_bh_ring_t bh_ring;
    volatile int bh_connected;
    volatile int bh_closed;
    volatile int bh_error;
    volatile int bh_err_code;
    volatile int bh_tx_wake;
    volatile int bh_pending;
} net_socket_t;

/*
 * One net_socket_t exists per open socket and up to a hundred of them are kept
 * alive by the socket obj_cache, so its size is a per-socket memory cost that
 * no runtime counter would show.  It used to embed 16 bottom-half events of
 * 64 KiB each, about 1.05 MiB per socket; the inline staging split in
 * net_bh_event_t brought the default profile to about 30 KiB.
 *
 * The bound is per profile rather than one global number so a profile with a
 * smaller ring or buffer is not forced to pay for the largest one, and it
 * exists so putting a fixed NET_MAX_PAYLOAD-sized member back fails the build
 * instead of quietly costing a megabyte per descriptor.
 */
_Static_assert(sizeof(net_socket_t) <= NET_PROFILE_SOCKET_MAX_BYTES,
               "net_socket_t exceeds the profile's per-socket budget; a fixed "
               "NET_MAX_PAYLOAD-sized staging member has crept back in");

#if CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_EMBEDDED

/*
 * The embedded tier's whole accounting, asserted here because this is the first
 * point in the include chain where net_socket_t is a complete type.
 *
 * The other three terms are checked against real sizeof() in the files that own
 * them -- socket_packet.c for the AF_PACKET ring, lwip_stack.c for the netif
 * state, netfilter_nat.c for the filter tables -- and the heap is a static
 * array of exactly NET_PROFILE_MEM_SIZE bytes in memp/mem.c.  What only this
 * file can check is the socket table, and it was the one term that overran the
 * part: eight net_socket_t at the old ring-4 / payload-320 geometry were 29120 B
 * against a 20 KiB device, which is the number docs/server-readiness.md kept
 * quoting as the reason the tier could not fit.
 *
 * With the per-tier ring depth and inline payload it is 8 x 2440 = 19520 B, so
 * the socket table is inside NET_PROFILE_SOCKET_BUDGET with 960 B to spare, and
 * the four terms together are 42644 B against NET_PROFILE_TOTAL_BUDGET (44 KiB).
 *
 * The frame term below is the macro bound, not the measured 3732 B: the AF_PACKET
 * ring carries one more slot than the profile's macro counts (the drain scratch
 * buffer in socket_packet.c), so the macro undercounts by 516 B and the real
 * total is 42644 rather than the 42128 the assert is checking.  That is the
 * right direction for a bound this tight and is why the ceiling carries 2.4 KiB
 * of slack.
 */
_Static_assert(NET_MAX_SOCKETS * sizeof(net_socket_t) <= NET_PROFILE_SOCKET_BUDGET,
               "the embedded profile's whole socket table does not fit the "
               "budget the profile declares for it; shrink the bottom-half ring "
               "depth or the inline payload rather than raising the ceiling");

_Static_assert(NET_MAX_SOCKETS * sizeof(net_socket_t) +
                   NET_PROFILE_PACKET_RING_SLOTS * NET_PROFILE_PACKET_SLOT_BYTES +
                   NET_PROFILE_NETIF_MAX_DEVS * NET_PROFILE_NETIF_STATE_BYTES +
                   NET_PROFILE_FILTER_BUDGET +
                   NET_PROFILE_MEM_SIZE
               <= NET_PROFILE_TOTAL_BUDGET,
               "the embedded profile's total RAM -- socket table, frame arrays, "
               "filter tables and the lwIP heap -- exceeds the ceiling the "
               "profile declares for itself; one of the four terms grew and the "
               "tier no longer fits the part it names");

#endif /* CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_EMBEDDED */

typedef struct sockaddr_alg_kernel {
    uint16_t family;
    uint8_t type[14];
    uint32_t feat;
    uint32_t mask;
    uint8_t name[64];
} sockaddr_alg_kernel_t;

/*
 * Socket locks: one per socket, plus one per run of registry slots.
 *
 * Two earlier generations are visible in this file.  g_net_lock covered the
 * whole registry in one spinlock, so every socket operation -- a send on one
 * connection, a poll on another, the /proc/net walk, a socket() -- serialized
 * on the same word.  2f17a5ba8 sharded that into net_bucket[]; stage E
 * (docs/net/net-lanes.md, "阶段 E") took the per-socket state off the shard,
 * because a shard is a run of NET_SOCK_SLOTS_PER_BUCKET slots and every socket
 * in that run still fought over one word.
 *
 * What each lock covers now, which is the whole point and the thing a future
 * edit must not blur:
 *
 *   net_bucket_t.lock   g_sockets[] -- publishing and clearing a registry slot,
 *                      net_bucket_t.free_bits, and nothing else.  Taken for slot
 *                      allocation (register) and slot lookup (walks).  A walk
 *                      nests the socket's own lock around whatever it reads out
 *                      of the object it found.
 *   net_socket_t.lock   every other field in net_socket_t: the receive queue,
 *                      the accept queue, the three wait queues, the state flags,
 *                      the addresses, the option ceilings.  This is the lock the
 *                      recv/send/close/accept hot paths take.
 *
 * The shard is a *contiguous* run of registry slots, not a hash: bucket =
 * reg_idx >> NET_SOCK_BUCKET_SHIFT, NET_SOCK_SLOTS_PER_BUCKET slots each.  That
 * choice is what makes the free bitmap workable.  The bitmap used to be one
 * flat uint32_t array indexed by slot, and both register and unregister did a
 * read-modify-write of a whole word; under a hash that word would hold slots
 * from up to 32 different buckets and two bucket locks would stomp on each
 * other.  With a contiguous run the free bits move inside the bucket as well
 * (net_bucket_t.free_bits), so one bitmap word belongs to exactly one bucket and
 * a register or unregister touches nothing another bucket lock protects.
 *
 * NET_SOCK_SLOTS_PER_BUCKET is 512 so the run is 16 whole uint32_t words and so
 * that the run divides both profile ceilings: the server profile's 65536 slots
 * become 128 buckets and the default profile's 1024 become 2.
 *
 * Lock order, outermost to innermost.  (Mirrored in kernel/include/core/lock.h,
 * whose network paragraph belongs to the file's owner.)
 *
 *   g_lwip_lock                     never held with either net lock, as before
 *   net_bucket[b]                   at most one at a time; taken only to reach
 *                                   the slot table
 *   net_socket_t.lock               any number, in ascending socket-pointer order
 *
 * The three rules that make that order work:
 *
 *   - A bucket lock is never taken while a socket lock is held.  This is the
 *     whole ABBA surface now, because the bucket lock is only ever reached for
 *     the slot table: net_register_socket_locked() and net_socket_unregister()
 *     acquire it themselves and callers must hold nothing when they call.
 *   - Two sockets are taken with net_sock_lock2(), which orders by address, so
 *     no pair is ever held in two directions.  Reading one socket's fields
 *     under only the *other* one's lock is not a cheaper version of this: it is
 *     the same race with fewer locks.
 *   - Never take the whole shard set to walk the table: with interrupts
 *     disabled, an interrupt that reaches a lookup then spins on a lock its own
 *     interrupted context is holding, and that livelock is recorded for the same
 *     mistake in fs/vfs/dcache.c.  Every walk goes one bucket at a time.
 *
 * g_lwip_lock is never held together with either net lock.  That is the
 * unchanged meaning of the old "g_lwip_lock and g_net_lock are never held
 * together" rule (docs/net/network-lock-contract.md).
 */
/*
 * Shard size, expressed as a shift of the global slot index.
 *
 * 512 slots per bucket on the server profile: 16 whole uint32_t bitmap words,
 * and 65536/512 = 128 buckets -- the same shape as the vfile table's 128.  The
 * size is derived from NET_MAX_SOCKETS rather than fixed, because the profiles
 * disagree by three orders of magnitude and the embedded profile's table is
 * only 8 slots: a hard-wired 512 would make NET_SOCK_BUCKETS zero there and
 * trip the divisibility assertion below.  Every profile's slot count is a power
 * of two, so each rung is exact.
 */
#if NET_MAX_SOCKETS >= 65536
#define NET_SOCK_BUCKET_SHIFT     9   /* 512 slots -> 128 buckets */
#elif NET_MAX_SOCKETS >= 1024
#define NET_SOCK_BUCKET_SHIFT     5   /*  32 slots ->  32 buckets */
#else
#define NET_SOCK_BUCKET_SHIFT     0   /*   1 slot  -> NET_MAX_SOCKETS buckets */
#endif
#define NET_SOCK_SLOTS_PER_BUCKET (1 << NET_SOCK_BUCKET_SHIFT)
/* The bitmap is always whole words, so a bucket narrower than one word (only
 * the 8-slot embedded profile reaches that) owns a word with its high bits
 * pinned occupied.  See net_bucket_claim() and net_socket_registry_init(). */
#define NET_SOCK_BUCKET_WORDS     ((NET_SOCK_SLOTS_PER_BUCKET + 31) / 32)
#define NET_SOCK_BUCKETS          (NET_MAX_SOCKETS / NET_SOCK_SLOTS_PER_BUCKET)

/* Canary written by net_socket_alloc() and cleared by the free that releases the
 * object; see the ref_magic comment in net_socket_t.  Any value but this one at
 * net_socket_free() time means the pointer is not a live socket. */
#define NET_SOCK_REF_MAGIC        0x4e534f4bu  /* 'N','S','O','K' */

_Static_assert((NET_MAX_SOCKETS % NET_SOCK_SLOTS_PER_BUCKET) == 0,
               "the socket-table shard size must divide the profile's slot "
               "ceiling, or a bucket would straddle the end of the slot space");
_Static_assert((NET_MAX_SOCKETS & (NET_MAX_SOCKETS - 1)) == 0,
               "the socket-table shard size is derived from NET_MAX_SOCKETS by "
               "halving, which is only exact when that is a power of two");
_Static_assert(NET_SOCK_BUCKET_WORDS * 32 >= NET_SOCK_SLOTS_PER_BUCKET,
               "a bucket's free bitmap must cover at least the bucket's slots");

typedef struct {
    spinlock_t lock;
    /* Bit clear == slot free, bit set == occupied: the polarity g_sock_free
     * had before the sharding ("bit n == 0 -> slot n is free"), which
     * net_bucket_claim() and net_socket_unregister() still use.  Only
     * this bucket's lock ever touches these words. */
    uint32_t   free_bits[NET_SOCK_BUCKET_WORDS];
} net_bucket_t;

extern net_bucket_t g_net_buckets[NET_SOCK_BUCKETS];
extern net_socket_t *g_sockets[NET_MAX_SOCKETS];
extern volatile int g_net_bh_pending[NET_MAX_SOCKETS];
extern volatile int g_net_bh_pending_count;
void net_bh_slot_mark(int idx);
int net_bh_slot_clear(int idx);

/* ------------------------------------------------------------------ *
 * Per-socket lock
 * ------------------------------------------------------------------ */

/*
 * Runtime probe for the net-lock side of the lock contract.
 *
 * g_lwip_lock got an executable assertion in 7d217d3fd (LWIP_ASSERT_CORE_LOCKED()
 * -> a20_lwip_assert_core_locked(), lwip_stack.c:760).  The net locks had none,
 * which left the two rules below as prose only:
 *
 *   - net_sock_lock2() takes two sockets in ascending address order and a
 *     critical section holds at most two socket locks;
 *   - a bucket lock is never taken while a socket lock is held (the bucket is
 *     the OUTER lock; net_bucket_scan() and the register/unregister pair nest
 *     the other way round, which is the sanctioned direction).
 *
 * The probe is a per-CPU set of the net locks this CPU currently holds.  It is
 * sound as a *per-CPU* set rather than a per-task one for a reason that is load
 * bearing and worth stating: every net lock is entered with spin_lock_irqsave()
 * and left with spin_unlock_irqrestore() (net_sock_lock2() takes its second
 * lock with plain spin_lock(), but only after the first acquire disabled
 * interrupts).  Interrupts stay off for the whole held window, so the holder can
 * neither be preempted by the timer nor interrupted into a nested acquisition
 * on the same CPU -- which also means it cannot migrate to another CPU while
 * holding one.  That is why one array indexed by cpu_current_id() is enough.
 *
 * Cost and switch, following the lwIP probe's shape (net_profile.h:37-46):
 *
 *   CONFIG_NET_LOCK_ASSERT == 0  the probe does not exist.  Nothing is counted,
 *                                 nothing can abort, /proc/net/status says
 *                                 "not checked" so a silent build is not read
 *                                 as a clean one.
 *   CONFIG_NET_LOCK_ASSERT == 1  a violation records its site and panics.
 *   CONFIG_NET_LOCK_ASSERT == 2  same bookkeeping, count only, no panic -- for
 *                                 a long soak where you want the violation count
 *                                 at the end rather than a dead machine on the
 *                                 first one.
 *
 * arming happens at the end of net_init() (socket.c), for the reason
 * g_lwip_lock_armed documents: nothing before that point may be judged, because
 * the registry locks do not exist yet.
 */
#if CONFIG_NET_LOCK_ASSERT
#define NET_LOCK_PROBE_SOCKET 0
#define NET_LOCK_PROBE_BUCKET 1

/* Deep enough for the contract's own ceiling (two socket + one bucket) plus one
 * slot of slack, so an overflow is reported as a violation instead of silently
 * overwriting a live entry. */
#define NET_LOCK_PROBE_MAX 4
#define NET_LOCK_PROBE_SITES 8
#endif

/*
 * Declared outside the switch because a20_lwip_format_status() renders a
 * "net_lock:" row in both builds -- the switch-off one says "not checked", so a
 * reader cannot mistake an absent probe for a clean run.  With the switch off
 * these resolve to the stubs at the bottom of net_lock_probe.c and no call site
 * in the lock helpers below reaches them.
 */
void net_lock_probe_acquire(const void *lock, int kind, const void *site);
void net_lock_probe_release(const void *lock, int kind);
void net_lock_probe_arm(void);
/* Renders the "net_lock:" row plus one "net_lock_siteN:" row per recorded site.
 * Returns the number of bytes it would have written, like the snprintf it uses. */
int net_lock_probe_format(char *buf, size_t bufsz);
unsigned net_lock_probe_violations(void);
/* Reported by net_socket_registry_init() when lock_counters_register() could not
 * fit a bucket lock; see net_lock_probe.c. */
void net_lockcounters_short(unsigned got, unsigned want, unsigned dropped);

static inline uint64_t net_sock_lock(net_socket_t *s)
{
    uint64_t flags = spin_lock_irqsave(&s->lock);
#if CONFIG_NET_LOCK_ASSERT
    net_lock_probe_acquire(&s->lock, NET_LOCK_PROBE_SOCKET,
                           __builtin_return_address(0));
#endif
    return flags;
}

static inline void net_sock_unlock(net_socket_t *s, uint64_t flags)
{
#if CONFIG_NET_LOCK_ASSERT
    /* Dropped before the spinlock is released, so the set never describes a
     * lock this CPU has already given up. */
    net_lock_probe_release(&s->lock, NET_LOCK_PROBE_SOCKET);
#endif
    spin_unlock_irqrestore(&s->lock, flags);
}

/*
 * Two sockets, always in ascending address order.  NULL means "no second
 * socket", which is how a peer back-pointer that turned out to be absent is
 * spelled; a == b collapses to one lock.
 *
 * The order is by address rather than by registry slot on purpose: a socket that
 * owns no slot -- the accepted end of an AF_UNIX stream, or one being created --
 * has no bucket to sort by at all, which is exactly why the orphan shard that
 * used to cover that case is gone.
 */
typedef struct {
    net_socket_t *lo;
    net_socket_t *hi;
    uint64_t      flags;
} net_sock_pair_t;

static inline net_sock_pair_t net_sock_lock2(net_socket_t *a, net_socket_t *b)
{
    net_sock_pair_t p;
    if (!a || !b || a == b) {
        p.lo = a ? a : b;
        p.hi = NULL;
    } else if ((uintptr_t)a < (uintptr_t)b) {
        p.lo = a;
        p.hi = b;
    } else {
        p.lo = b;
        p.hi = a;
    }
    p.flags = spin_lock_irqsave(&p.lo->lock);
    if (p.hi) {
        /* Interrupts are already off, so the second acquire's return value is
         * always 0 and must not be restored over the first unlock. */
        spin_lock(&p.hi->lock);
    }
#if CONFIG_NET_LOCK_ASSERT
    /* Recorded after both acquires rather than between them: the probe checks
     * the whole set (depth, ascending order, no repeats), so a half-populated
     * set would report a violation that the code does not have.  The ordering
     * this function exists to guarantee is therefore checked as a property of
     * the finished pair, not re-derived from the same comparison that built it
     * -- what the probe catches is a *third* lock arriving out of order, or a
     * single net_sock_lock() on a socket already held through another pair. */
    net_lock_probe_acquire(&p.lo->lock, NET_LOCK_PROBE_SOCKET,
                           __builtin_return_address(0));
    if (p.hi)
        net_lock_probe_acquire(&p.hi->lock, NET_LOCK_PROBE_SOCKET,
                               __builtin_return_address(0));
#endif
    return p;
}

static inline void net_sock_unlock2(net_sock_pair_t p)
{
#if CONFIG_NET_LOCK_ASSERT
    if (p.hi)
        net_lock_probe_release(&p.hi->lock, NET_LOCK_PROBE_SOCKET);
    net_lock_probe_release(&p.lo->lock, NET_LOCK_PROBE_SOCKET);
#endif
    if (p.hi)
        spin_unlock(&p.hi->lock);
    spin_unlock_irqrestore(&p.lo->lock, p.flags);
}

/* ------------------------------------------------------------------ *
 * Registry-slot lock
 * ------------------------------------------------------------------ */

/* Bucket that owns `s`, or -1 when it owns no slot.  Read reg_idx without a
 * lock: the only caller that still needs a bucket is net_socket_unregister(),
 * which is the code that clears the slot in the first place, and a live socket's
 * bucket never moves because it is registered exactly once.  The value is
 * written while both the bucket and the socket's own lock are held, so a
 * concurrent reader sees either the old slot or no slot, never a torn one. */
static inline int net_socket_bucket(const net_socket_t *s)
{
    int idx = s ? s->reg_idx : -1;
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return -1;
    return idx >> NET_SOCK_BUCKET_SHIFT;
}

static inline uint64_t net_bucket_lock(int b)
{
    uint64_t flags = spin_lock_irqsave(&g_net_buckets[b].lock);
#if CONFIG_NET_LOCK_ASSERT
    net_lock_probe_acquire(&g_net_buckets[b].lock, NET_LOCK_PROBE_BUCKET,
                           __builtin_return_address(0));
#endif
    return flags;
}

static inline void net_bucket_unlock(int b, uint64_t flags)
{
#if CONFIG_NET_LOCK_ASSERT
    net_lock_probe_release(&g_net_buckets[b].lock, NET_LOCK_PROBE_BUCKET);
#endif
    spin_unlock_irqrestore(&g_net_buckets[b].lock, flags);
}

/* Visit every registered socket of one bucket.  The bucket lock covers reading
 * the slot table and nothing else; each socket the scan lands on is visited
 * under its own lock, nested inside, so a walk sees the same per-socket state a
 * syscall would.  Returning true stops the scan; the return value is forwarded
 * to the caller so a "find one" search can bail out of the bucket loop.
 *
 * The nesting is safe because no interrupt path takes a socket lock: lwIP
 * callbacks run under g_lwip_lock and only stage into the per-socket ring.  A
 * visitor callback must still not block, allocate, or take g_lwip_lock. */
typedef bool (*net_bucket_slot_fn)(net_socket_t *s, int idx, void *arg);

static inline bool net_bucket_scan(int b, net_bucket_slot_fn fn, void *arg)
{
    int base = b << NET_SOCK_BUCKET_SHIFT;
    uint64_t flags = net_bucket_lock(b);
    bool stop = false;
    for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
        net_socket_t *s = g_sockets[base + k];
        if (!s)
            continue;
        uint64_t sflags = net_sock_lock(s);
        bool hit = fn(s, base + k, arg);
        net_sock_unlock(s, sflags);
        if (hit) {
            stop = true;
            break;
        }
    }
    net_bucket_unlock(b, flags);
    return stop;
}

/* Walk every bucket, one lock at a time. */
static inline bool net_table_scan_all(net_bucket_slot_fn fn, void *arg)
{
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        if (net_bucket_scan(b, fn, arg))
            return true;
    }
    return false;
}

/*
 * Reference count on net_socket_t.
 *
 * The registry holds one reference for as long as the socket is published in
 * g_sockets[], and any code that carries a socket pointer out of a lock holds
 * one of its own.  A lock used to do that job: a lookup found its target in the
 * table and used it without ever releasing the single global lock.  Once the
 * table was sharded, a lookup that found a socket and then needed another
 * socket's lock could not keep the first one held -- it would have to take them
 * in an order the finder did not know -- so the pointer has to outlive the lock
 * on its own.  That is now true of every net lock, and a socket's own lock is no
 * exception: net_bucket_slot_ref() is how a caller leaves the bucket behind.
 *
 * Nothing waits for this count to reach zero while holding a lock.  The last
 * reference is dropped after the last lock is released, because dropping it can
 * free the object (obj_cache_free(), then kfree for the AF_UNIX staging buffer)
 * and neither belongs in a spinlock critical section.
 *
 * This also closes a hole that predates the sharding: net_socket_from_file()
 * drops its vfile reference before returning, so a concurrent close() on
 * another CPU could free the socket before the caller got a lock at all.
 */
static inline net_socket_t *net_socket_ref(net_socket_t *s)
{
    if (!s)
        return NULL;
    __atomic_fetch_add(&s->refs, 1, __ATOMIC_ACQ_REL);
    return s;
}

/*
 * Reference-count ledger.
 *
 * docs/measured/impl-notes-net.md §8.3 and §8.6 recorded the reference count as
 * a primitive that had been walked by hand over 54 sites and never once run.
 * These four counters are that run, made visible:
 *
 *   allocs   sockets handed out by net_socket_alloc()
 *   frees    sockets whose last reference was dropped
 *   live     allocs - frees, i.e. sockets that exist and are not yet freed.  A
 *            create/destroy cycle that leaks shows up here as live climbing and
 *            never coming back down, which is the "不漏" half.
 *   faults   frees that arrived when the count said the object was already gone
 *            (refs <= 0) or when the canary did not match.  This is the "不重"
 *            half: a non-zero value means some path dropped one reference too
 *            many, which under an unchecked build is a silent double free.
 *
 * All four are counted unconditionally -- an alloc and a free already do an
 * atomic read-modify-write, so the marginal cost is three more relaxed
 * increments on a path that is nowhere near hot -- and rendered on
 * /proc/net/status.  CONFIG_NET_REF_ASSERT (net_profile.h) turns a fault from a
 * counter into a panic; the ledger itself is always there.
 */
extern volatile int g_net_sock_ref_allocs;
extern volatile int g_net_sock_ref_frees;
extern volatile int g_net_sock_ref_faults;
int net_sock_ref_live(void);
/* One "net_sock_ref:" row plus the per-fault-reason rows; returns the byte
 * count it would have written, like the snprintf it uses. */
int net_sock_ref_format(char *buf, size_t bufsz);

/*
 * -ENOTCONN attribution.
 *
 * impl-notes-net.md §8.5 left one question open: the peer is resolved with no
 * lock held and re-checked after the ordered pair is taken, so a concurrent
 * close() in that window turns a send into an error.  The mitigation (re-check
 * and fail) was in place; what was missing was any way to tell "the window
 * fired" from "this socket was never connected", because both surface as the
 * same errno.  On a 2%-failure bug whose whole difficulty is telling two
 * similar-looking things apart, that is not a cosmetic gap.
 *
 * Every -ENOTCONN return site therefore goes through net_notconn() with a
 * reason, and the three reasons that correspond to the §8.5 window are flagged
 * as such.  The /proc row separates "window" from "genuine" in one number:
 *
 *   net_notconn: total=<n> window=<n>
 *   net_notconn_<reason>: <n>
 *
 * `window` is the answer to the question §8.5 asked.  A soak that ends with
 * window=0 next to a large total is evidence the window is not reachable in
 * practice; a soak that ends with window climbing is evidence it is, and
 * without this counter the two would have been indistinguishable.
 *
 * net_notconn() is a real call rather than a macro because the counter is the
 * point; it sits on an error path, which is not hot.
 */
typedef enum {
    /* The §8.5 window: a peer resolved outside the lock failed its re-check. */
    NET_NOTCONN_SEND_TCP_NO_PEER = 0,
    NET_NOTCONN_SEND_TCP_PEER_GONE,
    NET_NOTCONN_BLOCKING_PRE_PARK_RACE,
    NET_NOTCONN_BLOCKING_POST_PARK_RACE,
    /* Ordinary "this socket has no connection" errors, for contrast. */
    NET_NOTCONN_SENDTO_NOT_CONNECTED,
    NET_NOTCONN_SEND_TCP_NOT_CONNECTED,
    NET_NOTCONN_SEND_TCP_SHUT_WR,
    NET_NOTCONN_GETPEERNAME,
    NET_NOTCONN_PEERPIDFD,
    NET_NOTCONN_VFS_WRITE_NO_PCB,
    NET_NOTCONN_ENQUEUE_META,
    NET_NOTCONN_ENQUEUE_PBUF,
    NET_NOTCONN_BLOCKING_PEER_MISMATCH,
    NET_NOTCONN__COUNT
} net_notconn_reason_t;

int net_notconn(net_notconn_reason_t why);
int net_notconn_window_total(void);
/* One "net_notconn:" row plus one row per non-zero reason. */
int net_notconn_format(char *buf, size_t bufsz);

/*
 * Pin one slot of the table long enough to leave the bucket lock behind it.
 *
 * This is the pattern every narrowed hot path uses: the bucket lock exists only
 * to read g_sockets[i], and everything that then happens to that socket runs
 * under the socket's own lock.  Without the reference the object could be freed
 * by a concurrent close() in the window between the two.
 *
 * Returns NULL when the slot is empty.  The caller drops the reference with
 * net_socket_free().
 */
static inline net_socket_t *net_bucket_slot_ref(int idx)
{
    int b = idx >> NET_SOCK_BUCKET_SHIFT;
    uint64_t flags = net_bucket_lock(b);
    net_socket_t *s = g_sockets[idx];
    if (s)
        net_socket_ref(s);
    net_bucket_unlock(b, flags);
    return s;
}

/*
 * Return true when the bounded deferred-wake batch filled before the queue
 * was drained.  The caller must drop the bucket lock, flush wake_q, and finish
 * the queue with wait_queue_wake_all().
 */
static inline bool
net_wait_queue_collect_all_locked(wait_queue_t *q,
                                  proc_wake_reason_t reason,
                                  proc_wake_q_t *wake_q)
{
    bool complete = false;
    (void)wait_queue_collect_all(q, 0, reason, wake_q, &complete);
    return !complete;
}

void     net_socket_registry_init(void);
uint16_t net_alloc_ephemeral_port_locked(void);
uint16_t net_ntohs(uint16_t x);
void     net_sockaddr_loopback(net_socket_t *s, uint16_t port);
int      net_sockaddr_port(const void *addr, size_t len, uint16_t *port);
void     net_sockaddr_set_port(void *addr, size_t len, uint16_t port);
int      net_sockaddr_in_local(const net_sockaddr_in_t *in);
int      net_sockaddr_to_lwip_ip(const void *addr, size_t len,
                                 ip_addr_t *ip, uint16_t *port);
unsigned net_socket_lane_of_addr(const void *addr, size_t len,
                                 unsigned fallback);
int      net_lwip_ip_to_sockaddr(const ip_addr_t *ip, uint16_t port,
                                 uint8_t out[NET_SOCKADDR_MAX],
                                 size_t *outlen);
net_socket_t *net_find_bound_socket(int domain, int type,
                                    const void *addr, size_t addrlen);
/*
 * Registry slot allocation.  Takes bucket locks itself, one at a time, and the
 * socket's own lock to publish in_registry / reg_idx, so the caller must hold
 * no net lock at all.  The trailing _locked in the name is the historical one:
 * it always named a function that took its own locks, and renaming it would have
 * churned every call site for no gain in meaning.
 */
int      net_register_socket_locked(net_socket_t *s);
/*
 * Release the registry slot.  Takes the socket's bucket and then the socket's own
 * lock, for the same reason: a bucket lock is never taken under a socket lock,
 * so unregistering from inside a per-socket critical section cannot be written
 * at all.  It returns without touching anything for a socket that owns no slot,
 * which is what makes the socket-free-without-a-slot case (an AF_UNIX stream
 * child) fall out without a special shard.
 *
 * The caller drops the registry's reference afterwards, outside every lock.
 */
/* Returns true when a registry slot was actually released, i.e. when the caller
 * still owes the registry's reference and must drop it with one more
 * net_socket_free().  False for a socket that never owned a slot -- an AF_UNIX
 * accepted child, or one mid-creation -- which carries only the creator's
 * reference.  Getting this wrong is a use-after-free in the teardown path; see
 * the comment on the definition in socket_registry.c. */
bool      net_socket_unregister(net_socket_t *s);
int      net_socket_is_valid_locked(net_socket_t *s);

/*
 * Liveness of a socket the caller already holds a reference to -- which is
 * every socket that reaches one of these call sites: the fd's own socket, a
 * peer's back-pointer, or a socket a table scan handed back with a reference.
 *
 * This is deliberately NOT net_socket_is_valid_locked().  That one asks "does
 * this socket still own a registry slot", which is the right question for a
 * socket a scan found in g_sockets[] but the wrong one for the accepted end of
 * an AF_UNIX stream: that child owns no slot by design (net_unix_socket_connect
 * leaves it with reg_idx == -1) while being entirely in use.  Asking it the
 * slot question there rejected the peer of every connected AF_UNIX socket, so
 * send() on one returned ECONNREFUSED and the connection the listener had
 * already accepted went unused.
 *
 * With a reference in hand the object cannot be freed under the caller, and
 * net_socket_close_file() sets ->closed under this socket's own lock, so "not
 * closed" is the whole of what liveness means here.  Read it under that lock,
 * or under the ordered pair when a peer is involved.
 */
static inline bool net_socket_is_live(const net_socket_t *s)
{
    return s && !s->closed;
}

/* Socket table enumeration for /proc/net/{tcp,tcp6,udp,udp6,unix}
 * (socket_table.c).  The walk takes one bucket lock at a time and runs the
 * callback under the visited socket's own lock, so the callback must not block,
 * allocate, or take g_lwip_lock.
 *
 * `family` narrows the walk to AF_INET or AF_INET6, or AF_UNSPEC for "either"
 * -- which is what /proc/net/tcp needs, because Linux's /proc/net/tcp lists
 * IPv4 only and /proc/net/tcp6 lists IPv6 only, and a kernel that mixed the
 * two into one file made every v6 row unreadable as a v6 row (it was rendered
 * with the tcp6 address layout inside a file whose siblings are all v4). */
typedef enum {
    NET_TABLE_TCP = 0,
    NET_TABLE_UDP,
    NET_TABLE_UNIX,
} net_table_kind_t;

typedef void (*net_table_visit_fn)(net_socket_t *s, void *arg);
int      net_socket_table_walk(net_table_kind_t kind, int family,
                               net_table_visit_fn fn, void *arg);

/* Total bytes currently readable, for ioctl(FIONREAD) on a socket.  Holds only
 * the socket's own lock.  Returns -ENOTSOCK for a non-socket and
 * -EOPNOTSUPP for a socket whose readability cannot be expressed as a byte
 * count (AF_PACKET). */
int      net_socket_rx_available(net_socket_t *s, size_t *out);

int      net_enqueue_msg_locked(net_socket_t *dst, const void *buf, size_t len,
                                const void *addr, size_t addrlen);
int      net_enqueue_msg_locked_fds(net_socket_t *dst, const void *buf,
                                    size_t len, const void *addr,
                                    size_t addrlen,
                                    vfile_t **files, int nfiles);
int      net_enqueue_msg_locked_meta(net_socket_t *dst, const void *buf, size_t len,
                                     const void *addr, size_t addrlen,
                                     const net_bh_event_t *meta);
int      net_enqueue_msg_locked_pbuf(net_socket_t *dst, const struct pbuf *p,
                                     uint32_t off, size_t len,
                                     const void *addr, size_t addrlen,
                                     const net_bh_event_t *meta);
int      net_enqueue_msg_blocking(net_socket_t *s, net_socket_t *dst, const void *buf, size_t len,
                                  const void *addr, size_t addrlen,
                                  int dontwait, uint64_t timeout_ticks);
int      net_dequeue_msg_locked(net_socket_t *s, void *buf, size_t len,
                                void *addr, size_t *addrlen);
int      net_dequeue_msg_locked_meta(net_socket_t *s, void *buf, size_t len,
                                     void *addr, size_t *addrlen,
                                     net_recv_meta_t *meta);
int      net_recvfrom_meta(int gfd, void *buf, size_t len, int flags,
                           void *addr, size_t *addrlen, net_recv_meta_t *meta);
int      net_recvfrom_socket_meta(net_socket_t *s, void *buf, size_t len,
                                  int flags, void *addr, size_t *addrlen,
                                  net_recv_meta_t *meta);
int      net_accept_queue_push_locked(net_socket_t *listener,
                                      net_socket_t *child);
net_socket_t *net_accept_queue_pop_locked(net_socket_t *listener);
net_msg_t    *net_msg_alloc(void);
void          net_msg_free(net_msg_t *m);
void          net_scm_drop_files(vfile_t **files, int nfiles);
net_socket_t *net_socket_alloc(void);
void          net_socket_free(net_socket_t *s);

int      net_task_has_unblocked_signal(task_t *t);
int      net_socket_wait_expired(net_socket_t *s, uint64_t start, int for_write);

void     net_alg_copy_string(char *dst, size_t dstsz,
                             const uint8_t *src, size_t srcsz);
int      net_alg_name_supported(const char *type, const char *name);
int      net_alg_socket_bind(net_socket_t *s, const void *addr, size_t addrlen);
int      net_alg_socket_accept(net_socket_t *s, size_t *addrlen, int flags);
int      net_alg_socket_send(net_socket_t *s, const void *buf, size_t len);
int      net_alg_socket_recv(net_socket_t *s, void *buf, size_t len);

int      net_unix_sockaddr_prepare(const void *addr, size_t addrlen,
                                   uint8_t out[NET_SOCKADDR_MAX],
                                   size_t *outlen,
                                   char *path_out, size_t path_outsz);
int      net_unix_path_parent_ok(const char *path);
int      net_unix_socket_bind(net_socket_t *s, const void *addr, size_t addrlen);
int      net_unix_socket_connect(net_socket_t *s, const void *addr, size_t addrlen);
int      net_unix_socket_sendto(net_socket_t *s, const void *buf, size_t len,
                                const void *addr, size_t addrlen);
int      net_unix_socket_sendto_fds(net_socket_t *s, const void *buf,
                                    size_t len, const void *addr,
                                    size_t addrlen,
                                    vfile_t **files, int nfiles);
int      net_netlink_bind(net_socket_t *s, const void *addr, size_t addrlen);
int      net_netlink_diag_request(net_socket_t *s, const void *buf, size_t len,
                                   const void *addr, size_t addrlen);
int      net_netlink_route_request(net_socket_t *s, const void *buf, size_t len,
                                   const void *addr, size_t addrlen);
int      net_netlink_uevent_send(net_socket_t *s, const void *buf, size_t len,
                                  const void *addr, size_t addrlen);
void     netlink_uevent_emit(const char *action, const char *subsystem,
                              const char *name, uint64_t devt);

/* RTNETLINK multicast (socket_netlink.c).  lwip_stack.c is the only producer,
 * and it learns of the change while holding g_lwip_lock -- so the event is
 * recorded as a pending flag and the notify functions below run after that
 * lock is dropped.  They must therefore be called with g_lwip_lock NOT held:
 * they take net locks, which the lock contract forbids holding together with
 * g_lwip_lock. */
typedef struct {
    uint32_t index;      /* netif index, as ifi_index / ifa_index */
    uint8_t  want_up;    /* the carrier/admin state the change settled on */
} nlrt_link_event_t;

void     net_netlink_link_notify(const nlrt_link_event_t *events, int n);
void     net_netlink_addr_notify(unsigned ifindex, const uint8_t addr[4],
                                 const uint8_t mask[4]);
/* The AF_INET6 counterpart, broadcast to RTNLGRP_IPV6_IFADDR.  Emitted by
 * a20_lwip_if_set_addr6() -- the only writer of an IPv6 address in this tree
 * besides the loopback netif's own ::1 -- so the group has a real event source
 * rather than being a defined number nothing can feed. */
void     net_netlink_addr6_notify(unsigned ifindex, const uint8_t addr[16],
                                  uint8_t prefixlen);

/* AF_PACKET raw L2 sockets (socket_packet.c).  The RX capture must stay
 * deferred: lwip_stack.c calls it holding g_lwip_lock, which is never held
 * together with a net lock. */
int      net_packet_socket_bind(net_socket_t *s, const void *addr, size_t addrlen);
int      net_packet_socket_send(net_socket_t *s, const void *buf, size_t len,
                                const void *addr, size_t addrlen);
void     net_packet_rx_defer(unsigned ifindex, const uint8_t *frame, size_t len);
void     net_packet_bottom_half_process(void);
int      net_packet_rx_pending(void);
int      net_packet_ifindex_by_name(const char *name);
/* Unconditional .bss owned by socket_packet.c (the capture ring plus its drain
 * copy), reported on /proc/a20/netmem so a tier's static footprint is readable
 * off a running system rather than only off a linker's symbol table. */
size_t   net_packet_static_bytes(void);

void     net_tcp_close_pcb(net_socket_t *s);
void     net_tcp_drop_pcb(net_socket_t *s);
int      net_inet_socket_init(net_socket_t *s);
void     net_inet_socket_destroy(net_socket_t *s);
void     net_inet_bottom_half_process_socket(net_socket_t *s);
void     net_inet_bottom_half_process_all(void);
int      net_inet_bind_pcb(net_socket_t *s, const void *addr, size_t addrlen);
int      net_inet_connect(net_socket_t *s, const void *addr, size_t addrlen,
                          const void *connect_addr, size_t peer_len);
int      net_inet_sendto(net_socket_t *s, const void *buf, size_t len,
                         int flags, const void *addr, size_t addrlen);
void     net_inet_accept_child_ready(net_socket_t *s);
/* Convert a bound AF_INET socket into a real lwIP LISTEN pcb so the port is
 * reachable from off-box.  No-op concept for AF_INET6, which still has no
 * LISTEN pcb path.  Takes g_lwip_lock internally. */
int      net_inet_tcp_listen(net_socket_t *s, int backlog);
/* Push the socket's IPPROTO_IP options into its pcb, and report the values its
 * packets actually carry when the caller never set them.  Shared rather than
 * forward-declared locally: defined in socket_inet.c, called from
 * socket_control.c. */
void     net_inet_ip_opts_apply(net_socket_t *s);
void     net_inet_ip_effective(net_socket_t *s, uint8_t *ttl, uint8_t *tos,
                               uint8_t *mc_ttl);

/* Select the congestion control algorithm (TCP_CONGESTION) on an lwIP pcb.
 * Defined in socket_inet.c, called from socket_control.c's setsockopt handler,
 * so an established connection can switch algorithms without the socket's
 * other options being reapplied.  Takes g_lwip_lock.  `alg` is a TCP_CONG_*
 * value; with LWIP_TCP_CUBIC off only the default is reachable. */
void     a20_net_cong_apply(struct tcp_pcb *pcb, uint8_t alg);

/* Re-apply the socket's SO_SNDBUF / SO_RCVBUF ceilings to an lwIP pcb.
 * Defined in socket_inet.c next to the doc comment that explains what each
 * option does and does not bound.  Takes g_lwip_lock.  Reached from
 * socket_control.c's setsockopt handler, from net_inet_tcp_apply_options() on
 * connect and on the accept path, so one code path owns the clamping for all
 * three entry points. */
void     net_inet_tcp_buf_apply(net_socket_t *s, struct tcp_pcb *pcb);

/* The largest SO_SNDBUF / SO_RCVBUF this socket could honour, ignoring what it
 * currently has.  `is_snd` selects which of the two.
 *
 * The stream and datagram ceilings are unrelated by construction -- a pcb's
 * capacity against this layer's staging and queue limits -- which is why this
 * cannot be a constant.  See the header comment on net_inet_tcp_buf_apply() for
 * what the stream ceiling is, and the NET_DGRAM_*_DEFAULT block above for what
 * the datagram one is.
 *
 * setsockopt clamps against this; nothing else should. */
uint32_t net_socket_buf_ceiling(net_socket_t *s, int is_snd);

/* The value actually in force: min(stored, ceiling).  This -- not the raw
 * field -- is what getsockopt reports and what the datagram send paths compare
 * against, so a report and an enforcement cannot drift apart. */
uint32_t net_socket_buf_in_force(net_socket_t *s, int is_snd);

net_socket_t *net_socket_from_file(int gfd);
int net_poll_file(vfile_t *vf, short events);
int      net_socket_install_file(net_socket_t *s, int flags);
void     net_event_notify(net_socket_t *s, uint32_t event, uint64_t data0,
                          uint64_t data1);
int      net_socket_close_file(vfile_t *vf);

void net_tcp_recved(net_socket_t *s, size_t len);

/* Internal IPC bridge (AF_UNIX socketpair): plain data flows through the
 * internal channel; SCM_RIGHTS messages fall back to the legacy queue.
 * Implemented in socket_unix.c, used by net/socket.c recv path. */
int unix_ch_recv(net_socket_t *s, void *buf, size_t len);
int unix_ch_peek(net_socket_t *s, void *buf, size_t len);
int unix_ch_send(net_socket_t *s, net_socket_t *dst, const void *buf, size_t len);

#endif /* _NET_SOCKET_INTERNAL_H */
