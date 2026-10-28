/*
 * C ABI of the Rust sendto transaction
 * (kernel/net/socket/socket_sendto.rs, selected by NET_SOCKET_SENDTO_IMPL=rust).
 *
 * Rust owns the decision surface: the family/type dispatch ladder, the
 * EPIPE-versus-ENOTSOCK rule for a torn-down stream, the destination
 * resolution with its four error answers, the queue-full handoff to the
 * blocking enqueue, and the chunk loop for a payload larger than one message.
 *
 * C owns the primitives: the flags check, the per-family send entries, the
 * netns test, the socket lock pair, the liveness test, the reference and the
 * free, the loopback autobind, the bound-socket scan, the two enqueues, the
 * read-wake collect and the wake queue.
 */
#ifndef _NET_SOCKET_SENDTO_STATE_H
#define _NET_SOCKET_SENDTO_STATE_H

#include <stddef.h>
#include <stdint.h>

#define A20_NET_SENDTO_SOCKADDR_MAX 128
#define A20_NET_SENDTO_MAX_PAYLOAD  65535

/* A borrowed view of the sending socket's fields. */
typedef struct net_sendto_view {
    void *s;
    void **net_ns;
    int *domain;
    int *sock_type;
    int *protocol;
    int *nonblock;
    int *closed;
    int *shut_wr;
    int *bound;
    int *connected;
    int *ever_connected;
    int *local_tcp;
    void **tcp;
    void **udp;
    void **raw;
    void **peer;
    uint8_t *local;
    size_t *local_len;
    uint8_t *peer_addr;
    size_t *peer_len;
    uint64_t *send_timeout_ticks;
} net_sendto_view_t;

/* net_sock_pair_t mirrored, so the ascending-address pair stays in C. */
typedef struct net_sendto_pair {
    void *lo;
    void *hi;
    uint64_t flags;
} net_sendto_pair_t;

void a20_net_sendto_view_init(void *s, net_sendto_view_t *out);
int  a20_net_sendto_flags_check(int flags);
int  a20_net_sendto_alg(void *s, const void *buf, size_t len);
int  a20_net_sendto_packet(void *s, const void *buf, size_t len, const void *addr,
                           size_t addrlen);
int  a20_net_sendto_netlink(void *s, const void *buf, size_t len,
                            const void *addr, size_t addrlen);
int  a20_net_sendto_ns_initial(void *s);
int  a20_net_sendto_raw_ipv6(void *s, const void *buf, size_t len,
                             const void *addr, size_t addrlen);
int  a20_net_sendto_inet(void *s, const void *buf, size_t len, int flags,
                         const void *addr, size_t addrlen);
int  a20_net_sendto_notconn(void *s);
int  a20_net_sendto_unix(void *s, const void *buf, size_t len, const void *addr,
                         size_t addrlen);

uint64_t a20_net_sendto_lock(void *s);
void a20_net_sendto_unlock(void *s, uint64_t flags);
int  a20_net_sendto_is_live(void *s);
void a20_net_sendto_autobind(void *s);
void *a20_net_sendto_ref(void *s);
void a20_net_sendto_free(void *s);
void a20_net_sendto_lock2(void *a, void *b, net_sendto_pair_t *out);
void a20_net_sendto_unlock2(const net_sendto_pair_t *in);
void *a20_net_sendto_find_bound(void *ns, int domain, int sock_type,
                                const void *addr, size_t addrlen);
int  a20_net_sendto_max_queue(void);
int  a20_net_sendto_enqueue_locked(void *dst, const void *buf, size_t len,
                                   const void *src, size_t src_len);
int  a20_net_sendto_enqueue_blocking(void *s, void *dst, const void *buf,
                                     size_t len, const void *src,
                                     size_t src_len, int dontwait,
                                     uint64_t timeout);
int  a20_net_sendto_collect_read(void *dst, void *wake_q);
void a20_net_sendto_wakeq_init(void *wake_q);
void a20_net_sendto_wakeq_flush(void *wake_q);

int  a20_net_sendto_sock(void *s, const void *buf, size_t len, int flags,
                         const void *addr, size_t addrlen);

#endif /* _NET_SOCKET_SENDTO_STATE_H */
