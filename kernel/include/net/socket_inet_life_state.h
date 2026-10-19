/*
 * C ABI of the Rust INET socket lifecycle
 * (kernel/net/socket_inet_life.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * Rust owns the per-type pcb creation and its callback wiring, the listen
 * conversion and its re-wiring, the teardown decision per pcb type (a listener
 * closes and purges its stage; a connection is unbound and closed gracefully
 * with abort as the fallback), the lane publication on init, and the release of
 * the bottom-half ring's spill references.  C owns the lwIP calls, the locks and
 * the lane primitives.
 */
#ifndef _NET_SOCKET_INET_LIFE_STATE_H
#define _NET_SOCKET_INET_LIFE_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_inet_life_view {
    void *s;
    void **net_ns;
    int *domain;
    int *sock_type;
    int *protocol;
    void **udp;
    void **raw;
    void **tcp;
} net_inet_life_view_t;

void a20_net_inet_life_view_init(void *s, net_inet_life_view_t *out);

void a20_net_inet_life_lock(uint64_t *flags);
void a20_net_inet_life_unlock(uint64_t flags);
int  a20_net_inet_life_ns_initial(void *ns);
unsigned a20_net_inet_life_lane_load(void *s);
void a20_net_inet_life_lane_store(void *s, unsigned lane);
void a20_net_inet_life_lane_enter(unsigned lane);
unsigned a20_net_inet_life_pcb_owner(void *pcb, int kind);
void *a20_net_inet_life_udp_new(int v6);
void  a20_net_inet_life_udp_cb(void *pcb, void *s);
void *a20_net_inet_life_raw_new(int v6, int protocol);
void  a20_net_inet_life_raw_cb(void *pcb, void *s);
void *a20_net_inet_life_tcp_new(int v6);
void  a20_net_inet_life_tcp_options(void *s, void *pcb);
void *a20_net_inet_life_tcp_listen(void *pcb, int backlog);
void  a20_net_inet_life_tcp_arg(void *pcb, void *s);
void  a20_net_inet_life_tcp_accept(void *pcb);
int   a20_net_inet_life_pcb_is_listen(void *pcb);
int   a20_net_inet_life_tcp_close(void *pcb);
void  a20_net_inet_life_tcp_abort(void *pcb);
void  a20_net_inet_life_tcp_unbind(void *pcb);
void  a20_net_inet_life_accept_purge(void *s);
void  a20_net_inet_life_udp_remove(void *pcb);
void  a20_net_inet_life_raw_remove(void *pcb);
int   a20_net_inet_life_spill_count(void);
void *a20_net_inet_life_spill_take(void *s, int i);
void  a20_net_inet_life_pbuf_free(void *p);

int  a20_net_inet_tcp_listen(void *s, int backlog);
int  a20_net_inet_socket_init(void *s);
void a20_net_inet_socket_destroy(void *s);

#endif /* _NET_SOCKET_INET_LIFE_STATE_H */
