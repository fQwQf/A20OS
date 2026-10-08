#ifndef _NET_LWIP_STACK_H
#define _NET_LWIP_STACK_H

#include "core/types.h"

/* LWIP_PROGRESS_API: use a20_lwip_poll() only through core/progress from
 * scheduler/idle code; socket paths may poll explicitly while waiting on network
 * state. */

void a20_lwip_init(void);
void a20_lwip_attach_netifs(void);
void a20_lwip_poll(void);
void a20_lwip_poll_waiter(void);
void a20_lwip_poll_locked(void);
/* Generic timeout/configuration work requires the cold barrier. Multi-lane
 * RX polling requires ingress and stages frames without protocol execution. */
void a20_lwip_poll_timers_locked(void);
void a20_lwip_poll_rx_locked(unsigned budget); /* budget 0 = no cap */
void a20_lwip_process_netif_irq_locked(int net_idx);
/* Stage D: the guaranteed receive poll point.  kernel_progress_run_bottom_halves()
 * (i.e. every sched() and idle pass, on every CPU) calls A20_LWIP_LANE_RX_POLL(),
 * so staged frames are processed without any task having to ask for them.
 *
 * Below two lanes the call site is not an empty function body -- it is absent
 * from the preprocessed source, for the same reason a20_lwip_lane_enter() below
 * is a macro (measured there, and re-measured here: at CONFIG_NET_LANES == 1 a
 * real call to an out-of-line empty poll adds ten bytes to progress.c's .text).
 * There is nothing staged at one lane: the device interrupt still hands every
 * frame straight to netif input as it always did. */
#if CONFIG_NET_LANES > 1
void a20_lwip_lane_rx_poll(unsigned budget);
#define A20_LWIP_LANE_RX_POLL(budget) a20_lwip_lane_rx_poll(budget)
#else
#define A20_LWIP_LANE_RX_POLL(budget) ((void)(budget))
#endif
/* Ingress protects device staging and netif topology, never protocol state.
 * It must not be acquired while holding a core lane. */
uint64_t a20_lwip_ingress_lock(void);
void a20_lwip_ingress_unlock(uint64_t flags);
void a20_lwip_signal_timer_pending(void);
uint64_t a20_lwip_lock(void);
void a20_lwip_unlock(uint64_t flags);
/* Control-plane writers acquire every core lane in ascending order.  Hot
 * callers hold one owner lane and may never upgrade to the control barrier. */
uint64_t a20_lwip_lane_lock(unsigned lane);
void a20_lwip_lane_unlock(uint64_t flags);
int a20_lwip_control_is_held(void);
int a20_lwip_lane_is_held(unsigned lane);
/* Select logical work ownership only while the requested lane is held.  Cold
 * callers own every lane; hot callers must retain their one owner context.
 * One-lane builds erase the context call at preprocessing time. */
#if CONFIG_NET_LANES > 1
void a20_lwip_lane_enter(unsigned lane);
#else
#define a20_lwip_lane_enter(lane) ((void)0)
#endif
int  a20_lwip_lock_is_held(void);
void a20_lwip_note_lock_violation(void *site);
unsigned a20_lwip_lock_violations(void);
int  a20_lwip_format_status(char *buf, size_t bufsz);
int  a20_lwip_format_route(char *buf, size_t bufsz);
int  a20_lwip_format_net_dev(char *buf, size_t bufsz);
int  a20_lwip_format_memp(char *buf, size_t bufsz);
int  a20_lwip_rx_pending_any(void);
void a20_lwip_signal_rx_pending(void);

/* AF_PACKET L2 access.  ifindex is the netif index (see
 * net_packet_ifindex_by_name in socket_internal.h); frame is a full
 * Ethernet frame including the 14-byte header. */
int  a20_lwip_packet_tx(unsigned ifindex, const uint8_t *frame, size_t len);
int  a20_lwip_if_hwaddr(unsigned ifindex, uint8_t out[8]);
int  a20_lwip_if_up(unsigned ifindex);
int  a20_lwip_if_default_index(void);

/* The IPv4 address of netif `net_idx` in host order, or 0 if it has none.
 * A negative index means "whichever netif currently has an address", for
 * callers that cannot pin an interface (netfilter's MASQUERADE).  Requires
 * g_lwip_lock. */
uint32_t a20_lwip_netif_ipv4(int net_idx);

/* Netif reconfiguration for the netlink write path.  Each takes g_lwip_lock
 * itself; callers must not already hold it.  A NULL mask or gw leaves that
 * field unchanged.  With LWIP_NETIF_API=0 only the primary IPv4 address is
 * reachable, so a non-primary IFA_LOCAL is refused with -EOPNOTSUPP rather
 * than silently overwriting the primary. */
int  a20_lwip_if_set_addr(unsigned ifindex, const uint8_t addr[4],
                          const uint8_t mask[4], const uint8_t gw[4]);
int  a20_lwip_if_get_addr(unsigned ifindex, uint8_t addr[4], uint8_t mask[4],
                          uint8_t gw[4]);
/* IPv6 counterpart of a20_lwip_if_set_addr(): add one address, which is all
 * lwIP with LWIP_NETIF_API=0 exposes.  There is no remove, so the netlink write
 * path refuses RTM_DELADDR for AF_INET6 rather than half-performing it, and the
 * address goes VALID without DAD because it came from an administrative
 * request rather than a router advertisement. */
int  a20_lwip_if_set_addr6(unsigned ifindex, const uint8_t addr[16],
                           uint8_t prefixlen);
int  a20_lwip_if_set_mtu(unsigned ifindex, uint16_t mtu);
int  a20_lwip_if_set_flags(unsigned ifindex, unsigned flags, unsigned mask);

#endif /* _NET_LWIP_STACK_H */
