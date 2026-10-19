/*
 * C ABI of the Rust local TCP connect pairing transaction
 * (kernel/net/socket_inet_connect.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * This is the third slice of socket_inet.c: the shortcut that pairs a
 * connecting stream socket with a listening one through the accept queue
 * instead of the stack.  It covers the ephemeral bind, the listener lookup and
 * its gate, the child's inheritance, the register-before-push order, the pair
 * wiring and the rollback when the push fails.
 *
 * The stack path (tcp_connect plus the blocking wait for the handshake) stays C:
 * the wait loop parks the task, and scheduling is not this module's to own.
 *
 * Rust owns the decisions: whether the ephemeral bind is needed, whether the
 * local shortcut applies at all (namespace and configured TCP path), whether
 * the listener's gate is open, what the child inherits, that the child is
 * registered before it is pushed, the pair wiring on success, and the full
 * rollback -- two references and the slot -- when the push fails.
 *
 * C owns the primitives: the socket lock and the address-ordered pair, the
 * ephemeral-port bind, the allocator, the listener search, the namespace
 * reference and the initial-namespace test, the configured TCP path, the
 * local-target test, the registry register/unregister, the accept-queue push,
 * the accept wakeup, the lwIP pcb drop, the wake-queue init/flush and the trace.
 */
#ifndef _NET_SOCKET_INET_CONNECT_STATE_H
#define _NET_SOCKET_INET_CONNECT_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Borrowed view of the fields the transaction reads and writes; the same view
 * type covers the connecting socket, the listener and the child. */
typedef struct net_inet_connect_view {
    void *s;
    void **net_ns;
    int *domain;
    int *sock_type;
    int *protocol;
    int *bound;
    int *connected;
    int *ever_connected;
    int *local_tcp;
    int *closed;
    int *listening;
    int *accept_count;
    uint8_t *local;
    size_t *local_len;
    uint8_t *peer_addr;
    size_t *peer_len;
    void **peer;
} net_inet_connect_view_t;

void a20_net_inet_connect_view_init(void *s, net_inet_connect_view_t *out);

/* The address-ordered pair, opaque here: C fills in the ordering. */
typedef struct net_inet_connect_pair {
    void *lo;
    void *hi;
    uint64_t flags;
} net_inet_connect_pair_t;

/* Primitives, defined in socket_inet.c. */
void a20_net_inet_connect_lock(void *s, uint64_t *flags);
void a20_net_inet_connect_unlock(void *s, uint64_t flags);
void a20_net_inet_connect_lock2(void *a, void *b, net_inet_connect_pair_t *pair);
void a20_net_inet_connect_unlock2(net_inet_connect_pair_t *pair);
int  a20_net_inet_connect_is_live(void *s);
void a20_net_inet_connect_bind_ephemeral(void *s);
void *a20_net_inet_connect_alloc(void);
void a20_net_inet_connect_free(void *s);
int  a20_net_inet_connect_sockaddr_port(const void *addr, size_t len,
                                        uint16_t *port);
int  a20_net_inet_connect_ns_initial(void *ns);
int  a20_net_inet_connect_tcp_path_fast(void);
int  a20_net_inet_connect_is_local_target(const void *addr, size_t len);
void *a20_net_inet_connect_find_listener(void *s, uint16_t port);
void a20_net_inet_connect_ns_put(void *ns);
void *a20_net_inet_connect_ns_get(void *ns);
int  a20_net_inet_connect_register(void *child);
int  a20_net_inet_connect_unregister(void *child);
int  a20_net_inet_connect_queue_push(void *listener, void *child);
void a20_net_inet_connect_wake_accept(void *listener, void *wake_q);
void a20_net_inet_connect_wakeq_init(void *wake_q);
void a20_net_inet_connect_wakeq_flush(void *wake_q);
void a20_net_inet_connect_drop_pcb(void *s);
void a20_net_inet_connect_trace(int which);

/* Entry point.  Returns 0 when the pair was wired, a negative errno when the
 * attempt failed after its own rollback, and 1 to mean "not handled locally,
 * continue with the stack path". */
int a20_net_inet_connect_local(void *s, const void *connect_addr, size_t peer_len,
                               int max_queue);

#endif /* _NET_SOCKET_INET_CONNECT_STATE_H */
