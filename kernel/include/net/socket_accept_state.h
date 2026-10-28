/*
 * C ABI of the Rust accept transaction
 * (kernel/net/socket/socket_accept.rs, selected by NET_SOCKET_ACCEPT_IMPL=rust).
 *
 * Rust owns the decision surface: the flag validation, the type/domain refusal,
 * the pop-or-block loop with its park and its re-check, the peer address
 * marshalling and the nonblock inheritance, and the rollback that runs when the
 * accepted child cannot be installed -- including the order in which the child
 * is marked closed, the peer's back-pointer is detached and its waiters are
 * drained, the registry slot is released with no socket lock held, and the
 * references are dropped.
 *
 * C owns the primitives: the algorithm accept, the queue pop, the socket lock
 * and its ascending-address pair, the park protocol, the liveness and signal
 * tests, the child field access, the install, the registry unregister, the
 * reference and the free, the wait-queue drains and wakes, and the inet
 * destroy.
 */
#ifndef _NET_SOCKET_ACCEPT_STATE_H
#define _NET_SOCKET_ACCEPT_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_accept_view {
    void *s;
    int *domain;
    int *sock_type;
    int *listening;
    int *nonblock;
    int *closed;
    uint64_t *recv_timeout_ticks;
    void **accept_head;
} net_accept_view_t;

/* net_sock_pair_t mirrored, so the ascending-address pair stays in C. */
typedef struct net_accept_pair {
    void *lo;
    void *hi;
    uint64_t flags;
} net_accept_pair_t;

void a20_net_accept_txn_view_init(void *s, net_accept_view_t *out);
int  a20_net_accept_txn_alg(void *s, size_t *addrlen, int flags);
uint64_t a20_net_accept_txn_lock(void *s);
void a20_net_accept_txn_unlock(void *s, uint64_t flags);
void *a20_net_accept_txn_pop(void *s);
int  a20_net_accept_txn_have_task(void);
int  a20_net_accept_txn_has_signal(void);
int  a20_net_accept_txn_wait_expired_locked(void *s, uint64_t start);
int  a20_net_accept_txn_park(void *s, uint64_t deadline);

void a20_net_accept_txn_child_ready(void *child);
size_t a20_net_accept_txn_child_peer_len(void *child);
void a20_net_accept_txn_child_peer_copy(void *child, void *addr, size_t n);
void a20_net_accept_txn_child_set_nonblock(void *child, int nonblock);
int  a20_net_accept_txn_install(void *child);

void a20_net_accept_txn_snapshot_peer(void *child, void **out_peer);
void a20_net_accept_txn_lock2(void *a, void *b, net_accept_pair_t *out);
void a20_net_accept_txn_unlock2(const net_accept_pair_t *in);
void a20_net_accept_txn_child_mark_closed(void *child);
int  a20_net_accept_txn_peer_detach(void *peer, void *child);
int  a20_net_accept_txn_peer_collect_read(void *peer, void *wake_q);
int  a20_net_accept_txn_peer_collect_write(void *peer, void *wake_q);
int  a20_net_accept_txn_unregister(void *child);
void a20_net_accept_txn_free(void *s);
void a20_net_accept_txn_child_destroy(void *child);
void a20_net_accept_txn_peer_wake_read(void *peer);
void a20_net_accept_txn_peer_wake_write(void *peer);
void a20_net_accept_txn_wakeq_init(void *wake_q);
void a20_net_accept_txn_wakeq_flush(void *wake_q);

int  a20_net_accept_sock(void *s, void *addr, size_t *addrlen, int flags);

#endif /* _NET_SOCKET_ACCEPT_STATE_H */
