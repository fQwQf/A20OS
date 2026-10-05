#ifndef _NET_LANE_H
#define _NET_LANE_H

/*
 * Network lanes.
 *
 * A lane is one CPU's private execution context for the network stack.  It owns
 * a receive queue, a pbuf pool, a timeout wheel, a filter shard, its statistics,
 * and the PCBs of the connections assigned to it.  A socket's PCB belongs to one
 * lane for its whole lifetime and never migrates, so the packet path touches only
 * state that lane already owns.
 *
 * Why this is the scaling axis: two network builds run out of different things.
 * A server runs out of lock throughput and cores, while an MCU runs out of RAM
 * and cannot afford an interrupt budget.  Those two axes have to stay separate,
 * so lane count (this file) and resource ceilings (net_profile.h) are independent
 * knobs.  Raising the lane count is what a server raises; an embedded build
 * leaves it at 1 and changes nothing else.
 *
 * The property that makes one codebase safe for both is that CONFIG_NET_LANES
 * == 1 must behave exactly as it did before lanes existed.  Every function here
 * folds to a constant 0 at that setting, so the embedded build compiles down to
 * today's code.  `smoke-net-lanes-n1` gates that by requiring a net_stress_test
 * checksum to match byte for byte between a 1-lane and a multi-lane build.
 *
 * Stages, and what each one is allowed to change (see docs/server-readiness.md
 * for why the ordering is not negotiable):
 *
 *   A  this file, and the lane field on net_socket_t.  No locking changes.
 *   B  lwIP's PCB lists bucketed by lane.  Requires A.  At 1 lane the buckets
 *      are index 0 and behaviour is unchanged.
 *   C  per-lane pbuf pools and timeout wheels.  Requires B, because the wheel
 *      is partitioned by which lane owns the PCB.
 *   D  receive drain hands packets to the owning lane instead of processing
 *      them inline.  Requires C.  The interrupt stages frames into per-lane
 *      queues; processing runs at a guaranteed poll point
 *      (kernel_progress_run_bottom_halves(), i.e. every sched() and idle pass)
 *      with the CPU that claims the lane declaring it.  g_lwip_lock is still
 *      one global lock, so this lands the dispatch and the move out of
 *      interrupt context, not the per-lane locking itself.
 *   E  per-socket lock replacing the socket-table shard locks.  DONE, ahead
 *      of D: the table is now sharded by slot run (g_net_lock is gone) and
 *      net_socket_t carries the refs refcount that used to be implicit in the
 *      single global lock.
 *
 * Do not skip a stage: B without C leaves one global timeout wheel behind the
 * per-lane PCBs, which reintroduces exactly the single-core serialization this
 * is meant to remove.
 */

#include "core/types.h"
#include "net/net_profile.h"

/*
 * Ownership hash.
 *
 * The same value must be computable in both directions of a connection:
 *
 *   - when the connection is set up, from its LOCAL (ip, port);
 *   - when a packet arrives, from (incoming source port, incoming destination
 *     address), because for an established connection the peer's source port is
 *     our local port and the peer's destination is our local address.
 *
 * That is why the port comes first and the address second: both sides agree on
 * which half is which.  Mixing the order, or hashing only the address, makes an
 * inbound packet land in a bucket that does not own the PCB.
 *
 * Deliberately not keyed on the socket pointer or an allocation counter: the
 * value must be reproducible from wire-visible fields alone, on a CPU that has
 * never seen the connection.
 *
 * `ip` is the raw network-order address for the family in question.  Callers
 * pass ip4_addr_get_u32() or ip6_addr_get_host_part(); mixing the two is a bug
 * the caller can only avoid by being explicit at the call site, so the two entry
 * points below are separate rather than one function with a flag.
 */
static inline unsigned net_lane_hash(uint32_t ip, uint16_t port)
{
    uint32_t h = (uint32_t)port * 2654435761u;
    h ^= ip;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}

#define net_lane_of(ip_u32, port) \
    ((unsigned)(net_lane_hash((uint32_t)(ip_u32), (uint16_t)(port)) % CONFIG_NET_LANES))

/* For state that has no port to key on -- ARP, ICMP, NDP -- so that even those
 * spread across lanes instead of landing on whichever CPU took the interrupt. */
#define net_lane_of_ip(ip_u32) \
    ((unsigned)(net_lane_hash((uint32_t)(ip_u32), 0) % CONFIG_NET_LANES))

/* Per-CPU lane for work that belongs to this CPU rather than to a connection:
 * packet filters keyed on nothing, and receive-drain accounting. */
#define net_lane_of_cpu(cpu) ((unsigned)((cpu) % CONFIG_NET_LANES))

#if CONFIG_NET_LANES < 1
#error "CONFIG_NET_LANES must be at least 1"
#endif

_Static_assert(CONFIG_NET_LANES <= 32,
               "lane indices are carried in u8_t and the PCB lane field is u8_t");

/*
 * Only ever a pointer to one, so a forward declaration is enough and this
 * header stays includable from inside lwIP (pcb_lane.h pulls it in).
 */
struct netif;

/*
 * Lane state is introduced in stage C, once there is something per-lane to own.
 * Declaring the array shape here keeps every later stage able to write
 * "per_lane[i]" against one definition, and keeps CONFIG_NET_LANES the only
 * place the count appears.
 *
 * Stage D adds the receive queue, and only under CONFIG_NET_LANES > 1.  At one
 * lane there is no dispatch to do -- a single consumer drains the device ring
 * inline, exactly as it did before lanes existed -- so the queue, its counters
 * and its claim flag are compiled out rather than present-but-unused.  That is
 * the same rule the rest of the tree follows: the one-lane build's preprocessed
 * source has to be the pre-lane source.
 */
/*
 * One staged frame.  Only ever a pointer to a netif and a byte buffer, so this
 * type pulls no kernel primitive into a header that lwIP includes.
 */
struct net_lane_rx_slot {
    struct netif *nif;
    uint16_t len;
    uint16_t reserved;
    uint8_t frame[NET_PROFILE_NETIF_FRAME_SIZE];
};

struct net_lane {
    unsigned index;
#if CONFIG_NET_LANES > 1
    /*
     * Received frames waiting for this lane's protocol processing.
     *
     * WHY FRAMES AND NOT PBUFS.  The producer runs in the device interrupt, and
     * pbuf_alloc() goes through memp, which has no internal locking and is only
     * safe because every caller holds g_lwip_lock.  Taking the core lock for a
     * pool allocation is exactly what stage D is trying to take off the packet
     * path, so the producer copies bytes into a preallocated slot instead and
     * leaves allocation to the consumer, which is in process context and holds
     * the lock anyway.  A slot is a fixed-size frame buffer plus the netif the
     * frame arrived on, because the consumer has to hand it to n->input() and
     * cannot rediscover which netif that was once the device ring is gone.
     *
     * head and tail are plain counters, not a ring of pointers: the slot array
     * is preallocated and the difference between the two is the queue, so
     * "full" is head - tail == depth rather than a NULL link to chase.
     *
     * ONE CONSUMER AT A TIME IS A CORRECTNESS REQUIREMENT, NOT A THROUGHPUT
     * ONE.  All packets of one connection land on this one lane, so two CPUs
     * draining it concurrently would feed the same pcb's input path in
     * whatever order they each happened to win -- reordered segments on a
     * stream socket, interleaved reassembly on a fragmented datagram.  The
     * consumer therefore claims the lane exclusively (net_lane_rx_claim) and
     * the queue is strictly FIFO for whoever holds the claim.
     */
    volatile uint32_t rx_head;   /* producer: published after the frame copy */
    volatile uint32_t rx_tail;   /* consumer: advanced as slots are popped */
    uint64_t rx_queued;          /* frames staged, never popped */
    uint64_t rx_processed;       /* frames handed to netif input */
    uint64_t rx_dropped;         /* frames refused because the lane was full */
    struct net_lane_rx_slot rxq[NET_PROFILE_LANE_RXQ_SLOTS];
#endif /* CONFIG_NET_LANES > 1 */
};

extern struct net_lane g_net_lanes[CONFIG_NET_LANES];

static inline struct net_lane *net_lane(unsigned index)
{
    return &g_net_lanes[index % CONFIG_NET_LANES];
}

/*
 * The current lane: which lane owns the network work in progress right now.
 *
 * Stage C needs this because lwIP's allocator API has no lane dimension:
 * memp_malloc(MEMP_PBUF) takes a pool id and nothing else, and pbuf_alloc()
 * has no lane parameter either.  The only way to partition a pool per lane
 * without rewriting every allocation site in the stack is for memp to ask
 * "which lane is this?" and index its pool array by the answer.
 *
 * WHY IT IS A CONTEXT AND NOT A HASH.  The obvious cheap answer is to derive
 * the lane from the CPU that happens to be running, and that answer is
 * forbidden here.  Lane already has exactly one definition in this tree --
 * net_lane_of(ip, port), address derived, and both ends of a connection must
 * compute the same value (see the header comment above).  A CPU-derived lane
 * is a *second* definition, and having two is what produced the two bugs
 * net-lanes.md records: 16304db8 (a pcb indexed by its lane twice) and
 * f7f3d670 (a lookup hashing the wrong port).  A pbuf pool keyed on the CPU
 * would put the pbufs of one connection in a different lane's pool from the
 * PCBs of the same connection, and the two halves would stop agreeing.
 * net_lane_of_cpu() exists for work that genuinely belongs to a CPU rather
 * than to a connection (ARP, ICMP, NDP -- see its comment), and must not be
 * used here.
 *
 * WHO SETS IT.  Every A20OS entry point into the lwIP core sets it before
 * calling into lwIP, and it stays valid for the whole g_lwip_lock critical
 * section:
 *
 *   - receive drain:  the netif being drained, by its own IPv4 address
 *     (a20_lwip_process_netif_rx_tx_locked).  Stage D replaces this with the
 *     owning lane of each individual packet, which is the real answer and is
 *     not available before a packet has been parsed.
 *   - socket system calls:  net_socket_t::lane, which is address derived from
 *     the bound (ip, port) by net_socket_lane_of_addr().
 *   - the timer segment:  whatever the caller established; it walks every
 *     lane, so it has no single owning lane.
 *
 * STORAGE.  One plain global, not a per-CPU array.  There is no per-CPU data
 * infrastructure in this tree yet (core/cpu.h only offers cpu_current_id()),
 * and a global is sufficient because the value is written and read only while
 * g_lwip_lock is held, which is what makes it a value at all.  The invariant
 * is stated here rather than assumed: reading it without the lock is a bug,
 * and a20_lwip_unlock() resets it to lane 0 so that a section which forgets
 * to set it degrades to lane 0 rather than inheriting a foreign lane.
 *
 * AT ONE LANE every function below compiles to a constant, so a caller can
 * write net_lane_ctx_push(lane) unconditionally and the embedded build folds
 * it away -- the same property net_lane_of() has.
 */#if CONFIG_NET_LANES > 1
extern unsigned a20_net_lane_cur;

/* Scope a stretch of lwIP work to one lane.  Returns the previous lane so a
 * nested section can restore it; see net_lane_ctx_pop(). */
static inline unsigned net_lane_ctx_push(unsigned lane)
{
    unsigned prev = a20_net_lane_cur;
    a20_net_lane_cur = lane % CONFIG_NET_LANES;
    return prev;
}

static inline void net_lane_ctx_pop(unsigned prev)
{
    a20_net_lane_cur = prev;
}

static inline unsigned net_lane_ctx_get(void)
{
    return a20_net_lane_cur;
}
#else /* CONFIG_NET_LANES == 1 */
static inline unsigned net_lane_ctx_push(unsigned lane)
{
    (void)lane;
    return 0;
}

static inline void net_lane_ctx_pop(unsigned prev)
{
    (void)prev;
}

static inline unsigned net_lane_ctx_get(void)
{
    return 0;
}
#endif /* CONFIG_NET_LANES > 1 */

/*
 * THE RECEIVE QUEUE (stage D).
 *
 * The device interrupt no longer runs the protocol stack.  It reads each frame
 * out of the device ring, works out which lane owns the connection it belongs
 * to, and copies it into that lane's queue; the protocol processing happens
 * later, in process context, on whichever CPU claims the lane.  The device
 * drain itself stays serial -- one ring, one lock, and the copy is the only
 * per-packet work left in interrupt context -- which is why this is a win on a
 * single-queue NIC without needing multiple queues (that is stage F).
 *
 * THE LANE OF AN INBOUND FRAME IS net_lane_of(dst_ip, dst_port), and it has to
 * be exactly that, because it is the same expression two other places use:
 *
 *   - lwIP's PCB lookup:  NET_PCB_LANE_OF(ip_current_dest_addr(), hdr->dest),
 *     which is the bucket the matching pcb is filed in;
 *   - the socket's own lane:  net_socket_lane_of_addr() on the bound address,
 *     which for an established connection is that same local address and port.
 *
 * The frame's destination address and port ARE the local connection's address
 * and port, so the value is reproducible from wire bytes on a CPU that has
 * never seen the connection, and both ends of a connection agree on it.  This
 * is also the resolution of the wildcard-listener asymmetry: a wildcard listener
 * is found from any lane (the sentinel bucket is probed by every lookup), and
 * the child connection a passive open produces carries the concrete
 * destination address, so from the second segment onward its packets hash to
 * the lane the child's socket says it belongs to.
 *
 * Two cases deliberately do not use the port, because reading a port out of
 * them would read payload:
 *
 *   - an IP fragment, whose transport header is only present in the first one.
 *     Every fragment of a datagram takes the address-only hash, so all of them
 *     land on the same lane and reassembly cannot be split across CPUs.
 *   - anything that is not IPv4/IPv6 over TCP or UDP (ARP, ICMP, NDP, ...),
 *     which is hashed by address for the reason net_lane_of_ip() gives.
 */
#if CONFIG_NET_LANES > 1

/* Producer side.  Requires g_lwip_lock, which is what serialises the queue
 * against the device IRQ on any CPU; the copy is to preallocated memory, so it
 * allocates nothing and is safe in interrupt context.  Returns false when the
 * lane is full, having counted the drop -- the frame is already off the device
 * at that point, so there is nothing to push back on. */
bool net_lane_rx_put(unsigned lane, struct netif *nif,
                     const uint8_t *frame, unsigned len);

/* Consumer side.  net_lane_rx_claim() is a non-blocking exclusive claim: it
 * returns false rather than waiting, because the poll point runs on every CPU
 * on every scheduler pass and must never queue behind another CPU's lane.
 * One holder at a time is what makes the queue's FIFO order the order the
 * protocol stack sees.  Callers must release before returning. */
int  net_lane_rx_claim(unsigned lane);
void net_lane_rx_release(unsigned lane);

/* Non-empty test, safe without the claim: a producer only ever adds. */
int  net_lane_rx_ready(unsigned lane);

/* Pop one frame.  Requires the claim; returns NULL when the lane is empty.  The
 * slot is released by this call, so the caller must finish with *frame -- copy
 * it into a pbuf -- before asking for the next one. */
struct netif *net_lane_rx_pop(unsigned lane, const uint8_t **frame,
                              unsigned *len);

/* Count one frame handed to netif input, for the per-lane /proc row. */
void net_lane_rx_count_processed(unsigned lane);

/* A cheap global gate for the guaranteed poll point: one relaxed load answers
 * "could any lane have work", which is what keeps an unconditional poll out of
 * the scheduler hot path's way when nothing is arriving.  It counts frames
 * staged and not yet popped, so it is raised by the producer itself rather than
 * by the event a blocked reader is waiting for -- see the liveness note in
 * docs/net/net-lanes.md, stage D. */
unsigned net_lane_rx_queued_total(void);

/* Counters for /proc.  Monotonic and read without the claim: a torn read of a
 * 64-bit counter is not a value any gate keys on, and taking the claim here
 * would make a diagnostic contend with the receive path. */
unsigned net_lane_rx_pending(unsigned lane);
unsigned long long net_lane_rx_stat(unsigned lane, int which);

enum {
    NET_LANE_RX_QUEUED = 0,
    NET_LANE_RX_PROCESSED,
    NET_LANE_RX_DROPPED,
};

#endif /* CONFIG_NET_LANES > 1 */

#endif /* _NET_LANE_H */
