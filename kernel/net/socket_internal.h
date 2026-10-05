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
 * a socket-table bucket lock and the allocator, and
 * docs/net/network-lock-contract.md forbids
 * both inside an lwIP callback.  So the callback only parks the pcb here and
 * the bottom half finishes the job, which is the same split the receive path
 * already uses.
 *
 * Bounded and lock-free in the shape of net_bh_ring_t: the producer runs with
 * local interrupts off under g_lwip_lock and publishes head with a release
 * fence, the consumer runs later under the listener's bucket lock.  `dropped`
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
     * Single-consumer guard for the drain, guarded by the listener's bucket
     * lock.  The ring has exactly one consumer because a slot's ownership
     * hands over when tail moves, and two drains reading the same tail would
     * both release the same slot.  The single g_net_lock used to provide that
     * exclusion implicitly; with the table sharded the drain has to drop the
     * listener's bucket (it registers children, which takes a shard of its
     * own), so the exclusion is explicit here.
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
     * is the pair an inbound packet can reproduce: the peer's source port is our
     * local port and the peer's destination is our local address.  Before bind
     * it is only a provisional assignment from the creating CPU, which is why
     * nothing may rely on it until the socket is bound.
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
     * written under that socket's bucket lock; the global counter it feeds,
     * g_pkt_bound_count, stays atomic because the lwIP receive path reads it
     * with g_lwip_lock held and never a bucket lock. */
    int pkt_bound_marked;
    int in_registry;
    int reg_idx;
    /* Reference count, see net_socket_ref() in this header. */
    volatile int refs;
    /*
     * Running receive-queue byte/message tally, formerly g_rxq_tally indexed by
     * registry slot -- 512 KiB of table on the server profile for a per-socket
     * number.  High 32 bits are the message count, low 32 the readable bytes;
     * see socket_queue.c for why the pair, not just the byte half, is what
     * makes it safe to trust.  Touched only under this socket's bucket lock.
     */
    uint64_t rxq_tally;
    int gfd;                       /* creating task's fd carrying this socket */
    struct vfile *vf;              /* the socket's vfile (EventQ identity) */
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

typedef struct sockaddr_alg_kernel {
    uint16_t family;
    uint8_t type[14];
    uint32_t feat;
    uint32_t mask;
    uint8_t name[64];
} sockaddr_alg_kernel_t;

/*
 * Socket-table shard locks.
 *
 * g_net_lock used to cover the whole registry in one spinlock, so every socket
 * operation -- a send on one connection, a poll on another, the /proc/net walk,
 * a socket() -- serialized on the same word.  The table is now sharded.
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
 * Lock rules (mirrored in kernel/include/core/lock.h, whose network paragraph
 * belongs to the file's owner):
 *
 *   - A per-socket critical section takes the one bucket that owns the socket.
 *   - A section that needs two sockets takes their two buckets in *ascending
 *     bucket order*, so no pair is ever held in two directions and the order is
 *     acyclic.  net_bucket_lock2() is the only sanctioned way to do it.
 *   - At most two bucket locks are held at a time.  Never take the whole set
 *     to walk the table: with interrupts disabled, an interrupt that reaches a
 *     lookup then spins on a lock its own interrupted context is holding, and
 *     that livelock is recorded for the same mistake in fs/vfs/dcache.c.
 *   - g_lwip_lock is never held together with any bucket lock.  That is the
 *     unchanged meaning of the old "g_lwip_lock and g_net_lock are never held
 *     together" rule (docs/net/network-lock-contract.md).
 *   - net_register_socket_locked() takes the bucket locks itself, one at a
 *     time, so it must NOT be called with another socket's bucket lock held:
 *     a bucket is shared by 512 slots, so nesting an arbitrary bucket under a
 *     held one is exactly the ABBA this ordering forbids.
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
/* A socket with reg_idx < 0 -- being created, or racing a concurrent close --
 * owns no slot and therefore no bucket.  Those accesses take this one extra
 * shard and re-check in_registry on entry; create and teardown are the only
 * traffic that ever reaches it.  It is the last index, so it sorts after every
 * real bucket in net_bucket_lock2(). */
#define NET_SOCK_ORPHAN_BUCKET    NET_SOCK_BUCKETS
#define NET_SOCK_BUCKET_COUNT     (NET_SOCK_BUCKETS + 1)

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
     * net_bucket_claim() and net_unregister_socket_locked() still use.  Only
     * this bucket's lock ever touches these words. */
    uint32_t   free_bits[NET_SOCK_BUCKET_WORDS];
} net_bucket_t;

extern net_bucket_t g_net_buckets[NET_SOCK_BUCKET_COUNT];
extern net_socket_t *g_sockets[NET_MAX_SOCKETS];
extern volatile int g_net_bh_pending[NET_MAX_SOCKETS];
extern volatile int g_net_bh_pending_count;
void net_bh_slot_mark(int idx);
int net_bh_slot_clear(int idx);

/* Bucket that owns `s`.  Read reg_idx without a lock on purpose: the value is
 * only a hint for picking a shard, and every caller re-checks s->in_registry
 * once it holds the shard it landed on (net_socket_is_valid_locked()).  A
 * socket is registered exactly once, so a live socket's bucket never moves. */
static inline int net_socket_bucket(const net_socket_t *s)
{
    int idx = s ? s->reg_idx : -1;
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return NET_SOCK_ORPHAN_BUCKET;
    return idx >> NET_SOCK_BUCKET_SHIFT;
}

static inline uint64_t net_bucket_lock(int b)
{
    return spin_lock_irqsave(&g_net_buckets[b].lock);
}

static inline void net_bucket_unlock(int b, uint64_t flags)
{
    spin_unlock_irqrestore(&g_net_buckets[b].lock, flags);
}

/* Two buckets, always ascending.  b < 0 means "no bucket to take". */
typedef struct {
    int      lo;
    int      hi;
    uint64_t flags;
} net_bucket_pair_t;

static inline net_bucket_pair_t net_bucket_lock2(int b1, int b2)
{
    net_bucket_pair_t p;
    if (b1 < 0 || b2 < 0 || b1 == b2) {
        p.lo = (b1 < 0) ? b2 : b1;
        p.hi = -1;
    } else if (b1 < b2) {
        p.lo = b1;
        p.hi = b2;
    } else {
        p.lo = b2;
        p.hi = b1;
    }
    p.flags = spin_lock_irqsave(&g_net_buckets[p.lo].lock);
    if (p.hi >= 0) {
        /* Interrupts are already off, so the second acquire's return value is
         * always 0 and must not be restored over the first unlock. */
        spin_lock_irqsave(&g_net_buckets[p.hi].lock);
    }
    return p;
}

static inline void net_bucket_unlock2(net_bucket_pair_t p)
{
    if (p.hi >= 0)
        spin_unlock(&g_net_buckets[p.hi].lock);
    spin_unlock_irqrestore(&g_net_buckets[p.lo].lock, p.flags);
}

/* Visit every registered socket of one bucket with that bucket's lock held.
 * Returning true stops the scan; the return value is forwarded to the caller so
 * a "find one" search can bail out of the bucket loop. */
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
        if (fn(s, base + k, arg)) {
            stop = true;
            break;
        }
    }
    net_bucket_unlock(b, flags);
    return stop;
}

/* Walk every bucket, one lock at a time.  The orphan shard is not part of the
 * table and is never scanned. */
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
 * g_sockets[], and any code that carries a socket pointer out of a bucket
 * critical section holds one of its own.  The bucket lock used to do that job:
 * a lookup found its target in the table and used it without ever releasing
 * the single global lock.  With the table sharded a lookup that finds a socket
 * in one bucket and then needs a second bucket cannot keep the first one held
 * -- it would have to take them in an order the finder did not know -- so the
 * pointer has to outlive the lock on its own.
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
int      net_register_socket_locked(net_socket_t *s);
void     net_unregister_socket_locked(net_socket_t *s);
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
 * leaves it with reg_idx == -1, so it lives in the orphan shard) while being
 * entirely in use.  Asking it the slot question there rejected the peer of
 * every connected AF_UNIX socket, so send() on one returned ECONNREFUSED and
 * the connection the listener had already accepted went unused.
 *
 * With a reference in hand the object cannot be freed under the caller, and
 * net_socket_close_file() clears the slot and sets ->closed inside one bucket
 * critical section, so "not closed" is the whole of what liveness means here.
 */
static inline bool net_socket_is_live(const net_socket_t *s)
{
    return s && !s->closed;
}

/* Socket table enumeration for /proc/net/{tcp,udp,unix} (socket_table.c).
 * The walk takes one bucket lock at a time and runs the callback under that
 * bucket's lock, so the callback must not block, allocate, or take
 * g_lwip_lock. */
typedef enum {
    NET_TABLE_TCP = 0,
    NET_TABLE_UDP,
    NET_TABLE_UNIX,
} net_table_kind_t;

typedef void (*net_table_visit_fn)(net_socket_t *s, void *arg);
int      net_socket_table_walk(net_table_kind_t kind,
                               net_table_visit_fn fn, void *arg);

/* Total bytes currently readable, for ioctl(FIONREAD) on a socket.  Holds only
 * the socket's own bucket lock.  Returns -ENOTSOCK for a non-socket and
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

/* AF_PACKET raw L2 sockets (socket_packet.c).  The RX capture must stay
 * deferred: lwip_stack.c calls it holding g_lwip_lock, which is never held
 * together with a socket-table bucket lock. */
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
 int      net_netlink_diag_request(net_socket_t *s, const void *buf, size_t len,
                                   const void *addr, size_t addrlen);
 int      net_netlink_uevent_send(net_socket_t *s, const void *buf, size_t len,
                                  const void *addr, size_t addrlen);
 void     netlink_uevent_emit(const char *action, const char *subsystem,
                              const char *name, uint64_t devt);

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
