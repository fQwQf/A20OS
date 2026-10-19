/*
 * C ABI of the Rust blocking receive transaction
 * (kernel/net/socket_recv.rs, selected by NET_SOCKET_RECV_IMPL=rust).
 *
 * Rust owns the decision surface: the MSG_WAITALL / MSG_PEEK / MSG_TRUNC /
 * MSG_DONTWAIT matrix, the channel-versus-legacy-queue choice and its
 * fd-delivery order, stream coalescing, the EOF and timeout ordering, the
 * lost-wakeup re-check and the accounting-before-wakeup ordering.
 *
 * C owns the primitives: the queue peek/dequeue copies, the channel read, the
 * socket and lwIP locks, the tick clock, the signal test, the park protocol
 * (proc_park_* plus the wait-queue link/commit/unlink), the wake-queue collects
 * and net_tcp_recved.
 */
#ifndef _NET_SOCKET_RECV_STATE_H
#define _NET_SOCKET_RECV_STATE_H

#include <stddef.h>
#include <stdint.h>

/* A borrowed view of the receiving socket's fields; the C bridge fills it and
 * Rust reads through the pointers while holding net_sock_lock. */
typedef struct net_recv_view {
    void *s;
    int *domain;
    int *sock_type;
    int *nonblock;
    int *closed;
    int *peer_closed;
    int *shut_rd;
    int *passcred;
    uint64_t *recv_timeout_ticks;
    void **ch_ep;
    void **rx_head;
    int *rx_count;
} net_recv_view_t;

/* proc_wake_q_t storage the Rust frame owns; must be at least
 * sizeof(proc_wake_q_t) and suitably aligned (the C bridge asserts it). */
#define A20_NET_RECV_WAKEQ_BYTES 2048

void a20_net_recv_view_init(void *s, net_recv_view_t *out);
int  a20_net_recv_flags_check(int flags);
int  a20_net_recv_alg(void *s, void *buf, size_t len);
uint64_t a20_net_recv_now(void);
void a20_net_recv_poll_waiter(void);
uint64_t a20_net_recv_lock(void *s);
void a20_net_recv_unlock(void *s, uint64_t flags);
int  a20_net_recv_have_task(void);
int  a20_net_recv_has_signal(void);
int  a20_net_recv_wait_expired(void *s, uint64_t start, int for_write);
int  a20_net_recv_wait_expired_locked(void *s, uint64_t start, int for_write);
size_t a20_net_recv_msg_avail(void *head);
int  a20_net_recv_dequeue(void *s, void *buf, size_t len, void *addr,
                          size_t *addrlen, void *meta);
void a20_net_recv_peek_msg(void *head, void *buf, size_t len, void *addr,
                           size_t *addrlen, void *meta, size_t *out_avail,
                           size_t *out_n);
int  a20_net_recv_channel(void *s, void *buf, size_t len, int peek);
void a20_net_recv_creds(void *s, void *meta);
int  a20_net_recv_collect_write(void *s, void *wake_q);
int  a20_net_recv_collect_read(void *s, void *wake_q);
void a20_net_recv_wakeq_init(void *wake_q);
void a20_net_recv_wakeq_flush(void *wake_q);
void a20_net_recv_recved(void *s, size_t len);

/*
 * The park protocol for one retry of the receive loop.  channel selects the
 * channel path's re-check (rx_head or channel readable) and its interrupt
 * errno (-EINTR, versus the legacy path's -ERESTARTSYS); dontwait is the
 * legacy re-check's extra condition.  Returns 0 to retry the loop, -EAGAIN
 * when the wait timed out or no task could park, or the path's interrupt
 * errno when the park was interrupted.
 */
int  a20_net_recv_park(void *s, uint64_t wait_deadline, int channel,
                       int dontwait);

int  a20_net_recvfrom_socket_meta_until(void *s, void *buf, size_t len,
                                        int flags, void *addr, size_t *addrlen,
                                        void *meta, uint64_t deadline);

#endif /* _NET_SOCKET_RECV_STATE_H */
