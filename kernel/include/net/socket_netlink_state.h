/*
 * C ABI of the Rust AF_NETLINK state machines
 * (kernel/net/control/socket_netlink.rs, selected by NET_SOCKET_NETLINK_IMPL=rust).
 *
 * Rust owns the bind transaction, the sock_diag request/response, the
 * kobject-uevent emitter and sender, the rtnetlink request/response and
 * multicast paths, and the per-bucket table walks whose filter decides who
 * receives what.
 *
 * C owns the primitives: the socket lock, the bucket lock and slot lookup, the
 * enqueue, the receive-queue byte count, the namespace reference and capability
 * test, the class-device registry, the lwIP netif snapshot and its mutation
 * helpers, the netif event indices and the klog.
 */
#ifndef _NET_SOCKET_NETLINK_STATE_H
#define _NET_SOCKET_NETLINK_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Borrowed view of the fields the transactions read. */
typedef struct net_netlink_view {
    void *s;
    void **net_ns;
    int *domain;
    int *sock_type;
    int *protocol;
    int *bound;
    int *connected;
    int *listening;
    uint8_t *local;
    size_t *local_len;
    uint8_t *peer_addr;
    size_t *peer_len;
} net_netlink_view_t;

void a20_net_netlink_view_init(void *s, net_netlink_view_t *out);

/* Primitives, defined in socket_netlink.c. */
void a20_net_netlink_lock(void *s, uint64_t *flags);
void a20_net_netlink_unlock(void *s, uint64_t flags);
int  a20_net_netlink_buckets(void);
int  a20_net_netlink_slots_per_bucket(void);
void a20_net_netlink_bucket_lock(int bucket, uint64_t *flags);
void a20_net_netlink_bucket_unlock(int bucket, uint64_t flags);
void *a20_net_netlink_slot_socket(int bucket, int i);
int  a20_net_netlink_is_live(void *s);
int  a20_net_netlink_enqueue(void *s, const uint8_t *buf, size_t len,
                             const void *src, size_t srclen);
uint32_t a20_net_netlink_rx_bytes(void *s);
uint32_t a20_net_netlink_local_pid(void *s);
uint32_t a20_net_netlink_local_groups(void *s);
uint32_t a20_net_netlink_host_pid(void);
int  a20_net_netlink_ns_initial(void *ns);
void *a20_net_netlink_current_ns(void);
int  a20_net_netlink_capable(void *ns);
void a20_net_netlink_class_emit_all(const char *action);
void *a20_net_netlink_class_get(const char *name);
const char *a20_net_netlink_class_subsystem(void *cdev);
uint64_t a20_net_netlink_class_devt(void *cdev);
void a20_net_netlink_class_put(void *cdev);
int  a20_net_netlink_snapshot(void *ns, void *out, int max);
uint32_t a20_net_netlink_event_index(const void *events, int i);
int  a20_net_netlink_if_set_mtu(uint32_t ifindex, uint16_t mtu);
int  a20_net_netlink_if_set_flags(uint32_t ifindex, uint32_t value, uint32_t mask);
int  a20_net_netlink_if_get_addr(uint32_t ifindex, uint8_t *ip, uint8_t *mask,
                                 uint8_t *gw);
int  a20_net_netlink_if_set_addr(uint32_t ifindex, const uint8_t *ip,
                                 const uint8_t *mask, const uint8_t *gw);
int  a20_net_netlink_if_set_addr6(uint32_t ifindex, const uint8_t *addr,
                                  uint8_t prefixlen, uint32_t valid, uint32_t preferred,
                                  int exclusive);
int  a20_net_netlink_if_del_addr6(uint32_t ifindex, const uint8_t *addr);
uint32_t a20_net_netlink_loopback_mtu(void *ns, uint32_t value, int set);
uint32_t a20_net_netlink_loopback_up(void *ns, uint32_t value, int set);
void a20_net_netlink_klog_uevent(const char *action, const char *subsystem,
                                 const char *name, uint32_t major,
                                 uint32_t minor, int delivered);
void a20_net_netlink_klog_drop(uint32_t group, int dropped, int delivered);

/* Entry points the C wrappers call. */
int  a20_net_netlink_bind(void *s, const void *addr, size_t addrlen);
int  a20_net_netlink_diag_request(void *requester, const void *buf, size_t len,
                                  const void *addr, size_t addrlen);
void a20_net_netlink_uevent_emit(const char *action, const char *subsystem,
                                 const char *name, uint64_t devt);
int  a20_net_netlink_uevent_send(void *requester, const void *buf, size_t len,
                                 const void *addr, size_t addrlen);
int  a20_net_netlink_route_request(void *requester, const void *buf, size_t len,
                                   const void *addr, size_t addrlen);
void a20_net_netlink_link_notify(const void *events, int n);
void a20_net_netlink_link_removed(const void *link);
void a20_net_netlink_addr_notify(uint32_t ifindex, const uint8_t addr[4],
                                 const uint8_t mask[4]);
void a20_net_netlink_addr6_notify(uint32_t ifindex, const uint8_t addr[16],
                                  uint8_t prefixlen, uint16_t msg_type);

#endif /* _NET_SOCKET_NETLINK_STATE_H */
