/*
 * C ABI of the Rust TCP stream send transaction
 * (kernel/net/backends/lwip/socket_inet_send_tcp.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * Rust owns the early refusals, the local socket-to-socket path (resolve,
 * reference, liveness, blocking enqueue, free) and the retry deadline policy.
 * The stack path's loop is Rust's too: the room computation, the tcp_write
 * halving retry, the output-failure rule, the allocation-blocked retry, and the
 * ordering around the lwIP lane lock.  The blocking wait itself -- link, re-check
 * room, try-wake, park commit -- stays a C primitive, because it is the
 * scheduler's protocol and this module must not own scheduling.
 */
#ifndef _NET_SOCKET_INET_SEND_TCP_STATE_H
#define _NET_SOCKET_INET_SEND_TCP_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_inet_send_tcp_view {
    void *s;
    int *connected;
    int *closed;
    int *ever_connected;
    int *shut_wr;
    int *local_tcp;
    int *nonblock;
    uint64_t *send_timeout_ticks;
    uint8_t *local;
    size_t *local_len;
    void **peer;
    void **tcp;
} net_inet_send_tcp_view_t;

void a20_net_inet_send_tcp_view_init(void *s, net_inet_send_tcp_view_t *out);

typedef struct net_inet_send_tcp_pair {
    void *lo;
    void *hi;
    uint64_t flags;
} net_inet_send_tcp_pair_t;

void a20_net_inet_send_tcp_lock(void *s, uint64_t *flags);
void a20_net_inet_send_tcp_unlock(void *s, uint64_t flags);
void a20_net_inet_send_tcp_lock2(void *a, void *b, net_inet_send_tcp_pair_t *pair);
void a20_net_inet_send_tcp_unlock2(net_inet_send_tcp_pair_t *pair);
int  a20_net_inet_send_tcp_is_live(void *s);
void a20_net_inet_send_tcp_ref(void *s);
void a20_net_inet_send_tcp_free(void *s);
int  a20_net_inet_send_tcp_notconn(int why);
int  a20_net_inet_send_tcp_enqueue_blocking(void *s, void *dst, const void *buf,
                                            size_t len, const uint8_t *src_local,
                                            size_t src_len, int nonblock,
                                            uint64_t timeout);
uint64_t a20_net_inet_send_tcp_now(void);
uint64_t a20_net_inet_send_tcp_ip_lane_lock(void *s, void *opts);
void a20_net_inet_send_tcp_lane_unlock(uint64_t flags);
void a20_net_inet_send_tcp_ip_opts_apply(void *s, const void *opts);
void a20_net_inet_send_tcp_poll_locked(void);
uint32_t a20_net_inet_send_tcp_snd_buf_max(void);
uint32_t a20_net_inet_send_tcp_snd_queuelen_max(void);
uint32_t a20_net_inet_send_tcp_pcb_snd_buf(void *tcp);
uint32_t a20_net_inet_send_tcp_pcb_snd_queuelen(void *tcp);
int  a20_net_inet_send_tcp_write(void *tcp, const uint8_t *buf, size_t len);
int  a20_net_inet_send_tcp_output(void *tcp);
void a20_net_inet_send_tcp_trace_deferred(int err);
int  a20_net_inet_send_tcp_wait(void *s, uint64_t start, int allocation_blocked);
void a20_net_inet_send_tcp_bottom_half_all(void);
void a20_net_inet_send_tcp_packet_bottom_half(void);

int a20_net_inet_send_tcp(void *s, const void *buf, size_t len);

#endif /* _NET_SOCKET_INET_SEND_TCP_STATE_H */
