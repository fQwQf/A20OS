#ifndef LWIP_PRIV_PCB_LANE_H
#define LWIP_PRIV_PCB_LANE_H

/*
 * Lane bucketing of the PCB lists.
 *
 * Every PCB list in the stack becomes an array of CONFIG_NET_LANES list heads,
 * and each PCB carries the index of the head it is linked into in its `lane`
 * field.  A lookup for an established connection then walks one list head
 * instead of one global list head, which is what makes the per-packet path
 * partitionable per lane.
 *
 * The index comes from the PCB's own (local_ip, local_port) through
 * net_lane_of(), with the port hashed before the address.  That order is
 * deliberate: for an established connection the peer's source port is our
 * local port and the peer's destination address is our local address, so both
 * ends of the connection derive the same value from wire bytes alone, on a CPU
 * that has never seen the connection.  The same expression serves a listening
 * pcb, because for an inbound segment the destination address and port *are*
 * the pcb's local address and port.
 *
 * A pcb bound to the "any" address matches a packet addressed to any local
 * address, and no hash of (0, port) can equal the hash of a concrete
 * destination: bind(0.0.0.0) means "all of them", not "pick one".  Those pcbs
 * are therefore not bucketed by the hash at all.  They are filed in a sentinel
 * bucket, and a lookup walks the hashed bucket first and the sentinel bucket
 * second.  A wildcard pcb lives in the sentinel bucket *only*: it must never be
 * linked into two lists, because the second insertion would overwrite its
 * ->next and silently corrupt both lists.
 *
 * The sentinel is only a bucket of its own once there is more than one lane.
 * With a single lane it coincides with lane 0, where a wildcard pcb belongs
 * anyway, and no second probe is made -- a single-lane build must not walk an
 * extra list, and it must compile to the code it compiled to before lanes
 * existed.
 *
 * THE SENTINEL IS A LOOKUP BUCKET, NOT AN OWNING LANE.  Two different questions
 * are answered by two different indices, and conflating them is what put the
 * "accept staging" blocker in net-lanes.md on stage D's critical path:
 *
 *   - which list head does a lookup walk?  NET_PCB_LANE_OF_PCB(), the sentinel
 *     for a wildcard pcb, because no hash of (0, port) can equal the hash of a
 *     concrete destination.
 *   - which lane owns the pcb's *work*?     NET_PCB_LANE_OWNER_OF_PCB(),
 *     always a real lane, because a lane index has to index something.
 *
 * Stage D dispatches received packets to the owning lane, so the second
 * question is the one a socket's net_socket_t::lane has to agree with.  For a
 * wildcard pcb the owning lane is the lane net_lane_of(0.0.0.0, local_port)
 * names -- which is exactly what net_socket_lane_of_addr() returns for a socket
 * that bound INADDR_ANY, because the wildcard address it hashes is zero too.
 * The two agree by construction rather than by a second rule, and the value is
 * still derived only from the socket's own bound address, so it needs no extra
 * field anywhere.
 */

#include "lwip/opt.h"
#include "lwip/ip_addr.h"

#include "net/net_lane.h"

#if CONFIG_NET_LANES > 1
/** Sentinel bucket for a pcb bound to the "any" address.  One past the last
 *  real lane, so it can never be mistaken for a lane index. */
#define NET_PCB_LANE_ANY        CONFIG_NET_LANES
/** Non-zero when a lookup has to walk the sentinel bucket as well as the
 *  hashed one.  It must be: a wildcard pcb is filed under the sentinel and
 *  nowhere else, so a lookup that skips the sentinel can never find it, and
 *  bind(0.0.0.0) -- the common case -- stops accepting connections. */
#define NET_PCB_LANE_ANY_PROBE  1
#else /* CONFIG_NET_LANES == 1 */
#define NET_PCB_LANE_ANY        0
#define NET_PCB_LANE_ANY_PROBE  0
#endif /* CONFIG_NET_LANES > 1 */

/** How many list heads each bucketed list has.  The sentinel is an extra head
 *  only when there is more than one lane; with a single lane it coincides with
 *  lane 0, so the array is exactly as long as it was before lanes existed and
 *  a one-lane build walks exactly the one list it always did. */
#define NET_PCB_LANE_BUCKETS    \
  (CONFIG_NET_LANES > 1 ? CONFIG_NET_LANES + 1 : CONFIG_NET_LANES)

/** How many buckets a search for a matching pcb walks: the hashed bucket, plus
 * the sentinel bucket when there is more than one lane.  A search loops over
 * this many buckets, taking the hashed one first and the sentinel one only as a
 * fallback, so a build with one lane walks exactly the one list it always did. */
#define NET_PCB_LANE_SEARCH_BUCKETS  (1 + NET_PCB_LANE_ANY_PROBE)

/** The 32 bits of an address that the lane hash keys on, in network byte
 * order: the whole address for IPv4, the low 32 bits for IPv6.  Both are wire
 * bytes, read the same way net_socket_lane_of_addr() reads them, so a socket's
 * lane and its PCB's lane are the same number rather than two hash functions
 * that have to be kept in agreement by hand. */
static inline u32_t
net_pcb_lane_ip(const ip_addr_t *ip)
{
#if LWIP_IPV6
  if (IP_IS_V6(ip)) {
    return ip_2_ip6(ip)->addr[3];
  }
#endif /* LWIP_IPV6 */
#if LWIP_IPV4
  return ip4_addr_get_u32(ip_2_ip4(ip));
#else /* !LWIP_IPV4 */
  return 0;
#endif /* !LWIP_IPV4 */
}

/** The bucket an address/port pair belongs to.  This is the single expression
 * that setup and input have to agree on. */
#define NET_PCB_LANE_OF(ipaddr, port) \
    ((u8_t)net_lane_of(net_pcb_lane_ip(ipaddr), (u16_t)(port)))

/** TRUE if this local address matches every incoming destination address, i.e.
 * the pcb is a catch-all.  This mirrors what the input paths test, so a pcb
 * this calls a catch-all is exactly a pcb the hashed bucket cannot hold. */
#define NET_PCB_IS_ANY_LOCAL_IP(ipaddr) \
    (IP_IS_ANY_TYPE_VAL(*(ipaddr)) || ip_addr_isany(ipaddr))

/** The bucket a pcb belongs in, derived from its own local address and port.
 * Call this only once local_ip and local_port are final: they are what the
 * bucket is derived from, and an inbound segment finds the pcb by recomputing
 * this same value from the wire.  Re-deriving it after a field changed is what
 * keeps a pcb from being left behind in a bucket that no lookup will ever
 * visit. */
#define NET_PCB_LANE_OF_PCB(pcb) \
    (NET_PCB_IS_ANY_LOCAL_IP(&(pcb)->local_ip) \
     ? (u8_t)NET_PCB_LANE_ANY \
     : NET_PCB_LANE_OF(&(pcb)->local_ip, (pcb)->local_port))

/** The real lane that owns a pcb filed in the sentinel bucket.  See the header
 *  comment: the sentinel answers "where do I look", this answers "whose work is
 *  it".  net_lane_of(0, port) is the owning lane of a wildcard bind, which is
 *  the same number the socket that performed that bind derived for itself. */
#define NET_PCB_LANE_OWNING_ANY(port) ((u8_t)net_lane_of(0, (u16_t)(port)))

/** The owning lane of any pcb: its hashed bucket when it has one, and the
 *  wildcard bind's own lane when it does not.  Always < CONFIG_NET_LANES, so it
 *  is safe to index per-lane state with; that is the whole difference from
 *  NET_PCB_LANE_OF_PCB(), which may return NET_PCB_LANE_ANY. */
#define NET_PCB_LANE_OWNER_OF_PCB(pcb) \
    (NET_PCB_IS_ANY_LOCAL_IP(&(pcb)->local_ip) \
     ? NET_PCB_LANE_OWNING_ANY((pcb)->local_port) \
     : NET_PCB_LANE_OF_PCB(pcb))

#endif /* LWIP_PRIV_PCB_LANE_H */
