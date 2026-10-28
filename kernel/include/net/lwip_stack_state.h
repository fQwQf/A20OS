/*
 * C ABI of the Rust netif-configuration and link-event state machine
 * (kernel/net/lwip_stack.rs, selected by NET_LWIP_STACK_IMPL=rust).
 *
 * This slice covers the part of lwip_stack.c that the socket and netlink layers
 * call: the netif address/flag/MTU transactions with their notification
 * decisions, the carrier-transition detector that batches link events, the
 * deferred link-event flush, and the AF_PACKET transmit entry.
 *
 * Rust owns the decisions: which validation refuses a write, what the address
 * notification carries when a slot is cleared, that the admin up/down event is
 * published synchronously while a carrier flip is deferred and coalesced, that
 * an IPv6 address is promoted to VALID without DAD, and the status mapping of a
 * device transmit.  C owns the lwIP netif access, the locks, the device ops, the
 * event collection and the netlink notify calls.
 */
#ifndef _NET_LWIP_STACK_STATE_H
#define _NET_LWIP_STACK_STATE_H

#include <stddef.h>
#include <stdint.h>

/* The pending link event, mirrored for Rust (same layout as nlrt_link_event_t). */
typedef struct a20_lwip_link_event {
    uint32_t index;
    uint8_t want_up;
} a20_lwip_link_event_t;

/* Primitives, defined in lwip_stack.c. */
void a20_lwip_stack_lock(uint64_t *flags);
void a20_lwip_stack_unlock(uint64_t flags);
void *a20_lwip_stack_netif_by_index(unsigned ifindex);

/* Prefix routes maintained by the Rust decision surface in lwip_stack.rs
 * (RTM_NEWROUTE).  Addresses are network byte order; 0 from lookup means
 * "no match", which is the signal for lwIP's own selection to run. */
int      a20_net_route_add(uint32_t dst, uint8_t prefix, uint16_t ifindex);
int      a20_net_route_del(uint32_t dst, uint8_t prefix, uint16_t ifindex);
void     a20_net_route_flush_if(uint16_t ifindex);
uint32_t a20_net_route_lookup(uint32_t dst);
int  a20_lwip_stack_netif_has_state(void *n);
void a20_lwip_stack_netif_hwaddr(void *n, uint8_t out[8]);
int  a20_lwip_stack_netif_is_up(void *n);
int  a20_lwip_stack_netif_is_link_up(void *n);
unsigned a20_lwip_stack_netif_index(void *n);
int  a20_lwip_stack_default_index(void);
void a20_lwip_stack_set_ip(void *n, const uint8_t addr[4]);
void a20_lwip_stack_set_mask(void *n, const uint8_t mask[4]);
void a20_lwip_stack_set_gw(void *n, const uint8_t gw[4]);
void a20_lwip_stack_get_ip(void *n, uint8_t out[4]);
void a20_lwip_stack_get_mask(void *n, uint8_t out[4]);
void a20_lwip_stack_get_gw(void *n, uint8_t out[4]);
int  a20_lwip_stack_add_ip6(void *n, const uint8_t addr[16]);
void a20_lwip_stack_set_mtu(void *n, uint16_t mtu);
void a20_lwip_stack_set_up(void *n, int up);
int  a20_lwip_stack_frame_size(void);
int  a20_lwip_stack_max_devs(void);
int  a20_lwip_stack_device_link_up(void *n);
void a20_lwip_stack_set_link(void *n, int up);
void a20_lwip_stack_link_pending_mark(void *n);
int  a20_lwip_stack_link_pending_any(void);
int  a20_lwip_stack_collect_link_events(void *out, int max);
void a20_lwip_stack_notify_link_events(const void *events, int n);
void a20_lwip_stack_notify_addr(unsigned ifindex, const uint8_t addr[4],
                                const uint8_t mask[4]);
void a20_lwip_stack_notify_addr6(unsigned ifindex, const uint8_t addr[16],
                                uint8_t prefixlen, uint16_t msg_type);
int  a20_lwip_stack_netif_sendable(void *n);
int  a20_lwip_stack_device_send(void *n, const uint8_t *frame, size_t len);

/* Entry points the C wrappers call. */
void a20_lwip_netif_sync_link_state(void *n);
void a20_lwip_netlink_flush_events(void);
int  a20_lwip_if_hwaddr(unsigned ifindex, uint8_t out[8]);
int  a20_lwip_if_up(unsigned ifindex);
int  a20_lwip_if_default_index(void);
int  a20_lwip_if_set_addr(unsigned ifindex, const uint8_t addr[4],
                          const uint8_t mask[4], const uint8_t gw[4]);
int  a20_lwip_if_get_addr(unsigned ifindex, uint8_t addr[4], uint8_t mask[4],
                          uint8_t gw[4]);
int  a20_lwip_if_set_addr6(unsigned ifindex, const uint8_t addr[16],
                           uint8_t prefixlen);
int  a20_lwip_if_del_addr6(unsigned ifindex, const uint8_t addr[16]);
int  a20_lwip_if_set_mtu(unsigned ifindex, uint16_t mtu);
int  a20_lwip_if_set_flags(unsigned ifindex, unsigned flags, unsigned mask);
int  a20_lwip_packet_tx(unsigned ifindex, const uint8_t *frame, size_t len);

#endif /* _NET_LWIP_STACK_STATE_H */
