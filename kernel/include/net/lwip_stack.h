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
/* Split so a caller that only advances timers does not drag the receive drain
 * into its critical section.  Both require g_lwip_lock held. */
void a20_lwip_poll_timers_locked(void);
void a20_lwip_poll_rx_locked(unsigned budget); /* budget 0 = no cap */
void a20_lwip_process_netif_irq_locked(int net_idx);
uint64_t a20_lwip_lock(void);
void a20_lwip_unlock(uint64_t flags);
/* Stage C: say which lane owns the lwIP work that is about to run, so lwIP's
 * allocator can index that lane's pbuf pool.  Call only with g_lwip_lock held
 * and only right after taking it; a20_lwip_unlock() clears it again, so there
 * is no matching exit call.
 *
 * At CONFIG_NET_LANES == 1 this has to disappear from the *preprocessed
 * source*, not merely from the assembly, and that is why it is a macro rather
 * than an out-of-line function or even a static inline:
 *
 *   - out of line, it exists at one lane too, so all ~19 call sites in
 *     socket_inet.c would gain a relocation and a call, to hold an empty body;
 *   - static inline with an empty body is still not enough.  Measured: with
 *     `(void)net_lane_ctx_push(lane)` at CONFIG_NET_LANES == 1, GCC's IR for
 *     socket_inet.c differs from HEAD's even though the instruction stream
 *     barely moves -- one function's stack-slot assignment shuffles and .text
 *     ends up four bytes shorter.  An empty inline still passes `s->lane` as an
 *     argument, and the argument's deadness is decided early enough to perturb
 *     register allocation.
 *
 * So at one lane the macro expands to nothing and `s->lane` is never read.  The
 * N=1 objects then differ from the pre-lane build only in UBSan's embedded
 * source-line table, which shifts because this file gained lines at all; the
 * generated code is byte-identical, verified by rebuilding these files with
 * -fno-sanitize=undefined on both trees. */
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
int  a20_lwip_if_set_mtu(unsigned ifindex, uint16_t mtu);
int  a20_lwip_if_set_flags(unsigned ifindex, unsigned flags, unsigned mask);

#endif /* _NET_LWIP_STACK_H */
