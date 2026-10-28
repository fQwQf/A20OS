/*
 * C ABI of the Rust socket-vfile lifecycle
 * (kernel/net/socket/socket_file.rs, selected by NET_SOCKET_FILE_IMPL=rust).
 *
 * Rust owns the close teardown transaction, the install transaction with its
 * rollback and the fd pin; C owns the primitives they call: the socket lock
 * (single and the address-ordered pair), the lwIP endpoint destroy, the channel
 * endpoint release, the wait-queue collect/wake/flush trio, the receive-queue
 * detach, the registry unregister, the socket free, the vfile allocator and the
 * fd-table install.  No struct ABI moves: the socket's fields are reached
 * through a borrowed view built here.
 */
#ifndef _NET_SOCKET_FILE_STATE_H
#define _NET_SOCKET_FILE_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Borrowed view of the fields the teardown transacts over.  Field order is the
 * contract with `CloseView` in kernel/net/socket/socket_file.rs. */
typedef struct net_close_view {
    void *s;
    int  *closed;
    int  *peer_closed;
    void **peer;
    const int *sock_type;
    void **accept_head;
    void **accept_tail;
    int  *accept_count;
    void **accept_next;
    /* net_socket_t.refs is volatile; the Rust side sees the same int*. */
    volatile int *refs;
    void **vf_priv;
} net_close_view_t;

void a20_net_close_view_init(void *s, net_close_view_t *out);

/* Primitives, defined in socket_file.c. */
void a20_net_close_lock(const net_close_view_t *v, uint64_t *flags);
void a20_net_close_unlock(const net_close_view_t *v, uint64_t flags);
void a20_net_close_lock_pair(const net_close_view_t *a,
                             const net_close_view_t *b,
                             const net_close_view_t **lo,
                             const net_close_view_t **hi, uint64_t *flags);
void a20_net_close_unlock_pair(const net_close_view_t *lo,
                               const net_close_view_t *hi, uint64_t flags);
void a20_net_close_inet_destroy(const net_close_view_t *v);
void a20_net_close_channel_release(const net_close_view_t *v);
void *a20_net_close_waitq(const net_close_view_t *v, int which);
int  a20_net_close_collect(void *q, int reason, void *wake_q);
void a20_net_close_wake(void *q, int reason);
void a20_net_close_flush(void *wake_q);
void a20_net_close_wake_q_reinit(void *wake_q);
void *a20_net_close_rxq_detach(const net_close_view_t *v);
void a20_net_close_msg_free_chain(void *m);
int  a20_net_close_unregister(const net_close_view_t *v);
void a20_net_close_socket_free(const net_close_view_t *v);

void *a20_net_install_prepare(void *s, int flags);
int   a20_net_install_fdtable(void *vf, int flags);
void  a20_net_install_set_owner(void *s, int gfd, void *vf);
void  a20_net_install_free(void *vf);
void *a20_net_fdtable_current_file(int fd);
void *a20_net_vfile_socket(void *vf);
void  a20_net_vfile_put(void *vf);
void  a20_net_socket_ref(void *s);

/* Entry points the C wrappers call. */
int a20_net_socket_close(const net_close_view_t *root, void *wake_q);
int a20_net_socket_install(void *s, int flags, void *wake_q);
int a20_net_socket_pin_current_fd(int fd, void **out_vf, void **out_sock);

#endif /* _NET_SOCKET_FILE_STATE_H */
