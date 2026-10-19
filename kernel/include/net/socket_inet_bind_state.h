/*
 * C ABI of the Rust INET bind transaction
 * (kernel/net/socket_inet_bind.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * Rust owns the per-type dispatch, the address conversion call, the pcb-present
 * check, the lane entry and the lane-store decisions on success, and the status
 * mapping.  C owns the lwIP lock, the lane primitives, the lwIP bind calls and
 * the address conversion.
 */
#ifndef _NET_SOCKET_INET_BIND_STATE_H
#define _NET_SOCKET_INET_BIND_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_inet_bind_view {
    void *s;
    void **net_ns;
    int *domain;
    int *sock_type;
    void **udp;
    void **raw;
    void **tcp;
} net_inet_bind_view_t;

void a20_net_inet_bind_view_init(void *s, net_inet_bind_view_t *out);

void a20_net_inet_bind_lock(uint64_t *flags);
void a20_net_inet_bind_unlock(uint64_t flags);
int  a20_net_inet_bind_ns_initial(void *ns);
int  a20_net_inet_bind_sockaddr_to_ip(const void *addr, size_t len, void *ip,
                                      uint16_t *port);
unsigned a20_net_inet_bind_lane_load(void *s);
void a20_net_inet_bind_lane_store(void *s, unsigned lane);
void a20_net_inet_bind_lane_enter(unsigned lane);
unsigned a20_net_inet_bind_pcb_owner(void *pcb, int kind);
unsigned a20_net_inet_bind_lane_of_addr(const void *addr, size_t len,
                                        unsigned fallback);
int  a20_net_inet_bind_udp(void *pcb, const void *ip, uint16_t port);
int  a20_net_inet_bind_raw(void *pcb, const void *ip);
int  a20_net_inet_bind_tcp(void *pcb, const void *ip, uint16_t port);

int a20_net_inet_bind_pcb(void *s, const void *addr, size_t addrlen);

#endif /* _NET_SOCKET_INET_BIND_STATE_H */
