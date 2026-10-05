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
 *      them inline.  Requires C.
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
 * Lane state is introduced in stage C, once there is something per-lane to own.
 * Declaring the array shape here keeps every later stage able to write
 * "per_lane[i]" against one definition, and keeps CONFIG_NET_LANES the only
 * place the count appears.
 */
struct net_lane {
    unsigned index;
};

extern struct net_lane g_net_lanes[CONFIG_NET_LANES];

static inline struct net_lane *net_lane(unsigned index)
{
    return &g_net_lanes[index % CONFIG_NET_LANES];
}

#endif /* _NET_LANE_H */
