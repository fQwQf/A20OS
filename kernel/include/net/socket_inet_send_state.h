/*
 * C ABI of the Rust UDP and raw send transactions
 * (kernel/net/backends/lwip/socket_inet_send.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * This is the fourth slice of socket_inet.c: the two datagram send paths.  The
 * stream send and the sendto dispatcher stay C.
 *
 * Rust owns the decisions: the SO_SNDBUF refusal, the lazy bind (v6 loopback
 * address versus the v4 any-address udp_bind and lane store), the destination
 * resolution (the connected peer's address, the stale back-pointer drop, the
 * table scan), the namespace decision, the local-delivery choice between a
 * blocking enqueue and an immediate one, the stack-path ordering (pbuf, address
 * conversion, the IP-option snapshot retry, the lane and per-protocol shared
 * locks, the option reapply, the connected-versus-addressed send choice, the
 * poll and the status mapping).
 *
 * C owns the primitives: the socket lock and pair, the reference and free, the
 * pbuf alloc/fill/free, the address conversion, the IP-option snapshot and its
 * reapply, the lane lock and the per-protocol shared lock, the lwIP send calls,
 * the receive-queue enqueue (immediate and blocking), the wakeup, the
 * namespace tests and the allocator.
 */
#ifndef _NET_SOCKET_INET_SEND_STATE_H
#define _NET_SOCKET_INET_SEND_STATE_H

#include <stddef.h>
#include <stdint.h>
#include "net/datagram.h"

/* Borrowed view of the fields the transactions read and write. */
typedef struct net_inet_send_view {
    void *s;
    void **net_ns;
    int *domain;
    int *bound;
    int *connected;
    int *nonblock;
    uint64_t *send_timeout_ticks;
    int *rx_count;
    uint8_t *local;
    size_t *local_len;
    uint8_t *peer_addr;
    size_t *peer_len;
    void **peer;
    void **udp;
    void **raw;
} net_inet_send_view_t;

void a20_net_inet_send_view_init(void *s, net_inet_send_view_t *out);

/* The address-ordered pair, opaque here: C fills in the ordering. */
typedef struct net_inet_send_pair {
    void *lo;
    void *hi;
    uint64_t flags;
} net_inet_send_pair_t;

/* Primitives, defined in socket_inet.c. */
void a20_net_inet_send_lock(void *s, uint64_t *flags);
void a20_net_inet_send_unlock(void *s, uint64_t flags);
void a20_net_inet_send_lock2(void *a, void *b, net_inet_send_pair_t *pair);
void a20_net_inet_send_unlock2(net_inet_send_pair_t *pair);
int  a20_net_inet_send_is_live(void *s);
void a20_net_inet_send_ref(void *s);
void a20_net_inet_send_free(void *s);
uint32_t a20_net_inet_send_buf_in_force(void *s);
uint32_t a20_net_inet_send_tcp_autotune_grow(void *s, void *tcp);
void     a20_net_inet_send_tcp_autotune_store(void *s, uint32_t value);
uint16_t a20_net_inet_send_ephemeral_port(void);
int a20_net_inet_send_bind_v6(void *s, uint16_t port);
int a20_net_inet_send_bind_v4(void *s, uint16_t port);
void *a20_net_inet_send_find_udp_dst(void *s, const void *addr, size_t len);
int  a20_net_inet_send_namespace_target(void *s, const void *addr, size_t len);
int  a20_net_inet_send_ns_initial(void *s);
int  a20_net_inet_send_enqueue_locked(void *dst, const void *buf, size_t len,
                                      const uint8_t *src_local, size_t src_len);
int  a20_net_inet_send_enqueue_blocking(void *s, void *dst, const void *buf,
                                        size_t len, const uint8_t *src_local,
                                        size_t src_len, int dontwait,
                                        uint64_t timeout);
void a20_net_inet_send_collect_read(void *dst, void *wake_q);
void a20_net_inet_send_wakeq_init(void *wake_q);
void a20_net_inet_send_wakeq_flush(void *wake_q);
void *a20_net_inet_send_pbuf_alloc(size_t len);
void a20_net_inet_send_pbuf_take(void *p, const void *buf, size_t len);
void a20_net_inet_send_pbuf_free(void *p);
int  a20_net_inet_send_sockaddr_to_ip(const void *addr, size_t len, void *ip,
                                      uint16_t *port);
uint64_t a20_net_inet_send_ip_lane_lock(void *s, void *opts);
void a20_net_inet_send_lane_unlock(uint64_t flags);
void a20_net_inet_send_ip_opts_apply(void *s, const void *opts);
int  a20_net_inet_send_udp_sendto(void *pcb, void *p, const void *ip, uint16_t port);
int  a20_net_inet_send_udp_send(void *pcb, void *p);
int  a20_net_inet_send_udp_connected(void *pcb);
int  a20_net_inet_send_raw_sendto(void *pcb, void *p, const void *ip);
int  a20_net_inet_send_raw_send(void *pcb, void *p);
int  a20_net_inet_send_raw_connected(void *pcb);
uint64_t a20_net_inet_send_shared_lock(int kind, void *pcb);
void a20_net_inet_send_shared_unlock(int kind, void *pcb, uint64_t flags);
void a20_net_inet_send_poll_locked(void);

/* Entry points. */
int a20_net_inet_send_udp(void *s, const void *buf, size_t len, int flags,
                          const void *addr, size_t addrlen, int max_queue);
int a20_net_inet_send_udp_control(void *s, const void *buf, size_t len, int flags,
    const void *addr, size_t addrlen, int max_queue,
    const net_datagram_control_t *control);
int a20_net_inet_send_udp_control_packet(void *pcb, void *p, const void *dest,
    uint16_t port, uint32_t ifindex, uint32_t source);
int a20_net_inet_send_raw(void *s, const void *buf, size_t len,
                          const void *addr, size_t addrlen, int max_queue);

#define A20_NET_SEND_SHARED_UDP 0
#define A20_NET_SEND_SHARED_RAW 1

#endif /* _NET_SOCKET_INET_SEND_STATE_H */
