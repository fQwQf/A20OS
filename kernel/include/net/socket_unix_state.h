/*
 * C ABI of the Rust AF_UNIX state machine
 * (kernel/net/socket_unix.rs, selected by NET_SOCKET_UNIX_IMPL=rust).
 *
 * Rust owns the address preparation, the bind transaction with its rollback,
 * the connect transaction that allocates and wires the accepted child, the
 * sendto transaction with its destination resolution and stream fragmentation,
 * and the channel-backed data plane (the chunked send, the stream staging loop
 * and the peek that must not dequeue).
 *
 * C owns the primitives they call: the socket lock (single and the
 * address-ordered pair), the liveness test, the reference and the free, the
 * socket allocator, the whole-table bound-socket scan, the current task's
 * credentials and cwd, the vfs stat/open/close that materialize a bound path,
 * the accept-queue push and the wakeup trio, the channel endpoint release and
 * the two channel calls, the stream-buffer allocator, the legacy receive-queue
 * enqueue and the wake-queue init/flush.
 *
 * No struct ABI moves: the socket's fields are reached through a borrowed view
 * built here, and the lock discipline is unchanged (the pair is taken in
 * ascending address order by net_sock_lock2(), the table scan runs with no net
 * lock held and hands back a reference).
 */
#ifndef _NET_SOCKET_UNIX_STATE_H
#define _NET_SOCKET_UNIX_STATE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Borrowed view of the fields the transactions read and write.  Field order is
 * the contract with `UnixView` in kernel/net/socket_unix.rs; the same view type
 * is used for the socket under operation, the listener and the accepted child,
 * because all three are net_socket_t.
 */
typedef struct net_unix_view {
    void *s;
    void *net_ns;
    int *domain;
    int *sock_type;
    int *protocol;
    int *nonblock;
    int *bound;
    int *connected;
    int *ever_connected;
    int *listening;
    int *accept_count;
    int *passcred;
    int32_t *owner_pid;
    int32_t *owner_uid;
    int32_t *owner_gid;
    int32_t *peer_pid;
    int32_t *peer_uid;
    int32_t *peer_gid;
    int32_t *ch_cred_pid;
    int32_t *ch_cred_uid;
    int32_t *ch_cred_gid;
    uint8_t *local;
    size_t *local_len;
    uint8_t *peer_addr;
    size_t *peer_len;
    void **peer;
    void **accept_next;
    void **ch_ep;
    char **ch_buf;
    uint32_t *ch_len;
} net_unix_view_t;

void a20_net_unix_view_init(void *s, net_unix_view_t *out);

/* Primitives, defined in socket_unix.c. */
void a20_net_unix_lock(void *s, uint64_t *flags);
void a20_net_unix_unlock(void *s, uint64_t flags);
/*
 * The two-socket lock is handed back to Rust as an opaque (lo, hi, flags)
 * triple: the ascending-address ordering stays in net_sock_lock2() alone
 * instead of being re-derived on the Rust side.
 */
typedef struct net_unix_pair {
    void *lo;
    void *hi;
    uint64_t flags;
} net_unix_pair_t;

void a20_net_unix_lock2(void *a, void *b, net_unix_pair_t *out);
void a20_net_unix_unlock2(net_unix_pair_t *in);
int  a20_net_unix_is_live(void *s);
void a20_net_unix_ref(void *s);
void a20_net_unix_free(void *s);
void *a20_net_unix_alloc(void);
void *a20_net_unix_find_bound(void *ns, int domain, int sock_type,
                             const uint8_t *addr, size_t addrlen);
int  a20_net_unix_creds(int32_t *pid, int32_t *uid, int32_t *gid);
void a20_net_unix_cwd(char *out, size_t outsz);
int  a20_net_unix_path_is_dir(const char *path);
/* Raw vfs_stat result: 0 when the path exists, -errno otherwise.  Kept as the
 * stat result rather than an "exists" flag so the caller's `== 0` test reads
 * the same way it does in C. */
int  a20_net_unix_path_stat(const char *path);
int  a20_net_unix_path_create(const char *path);
void a20_net_unix_path_close(int fd);
int  a20_net_unix_accept_push(void *listener, void *child);
int  a20_net_unix_accept_collect(void *listener, void *wake_q);
int  a20_net_unix_read_collect(void *s, void *wake_q);
void a20_net_unix_read_wake_all(void *s);
void a20_net_unix_channel_release(const net_unix_view_t *v);
int64_t a20_net_unix_channel_send(void *ep, const uint8_t *buf, uint32_t len,
                                  uint32_t flags);
int64_t a20_net_unix_channel_recv(void *ep, uint8_t *buf, uint32_t *cap);
uint8_t *a20_net_unix_chbuf(const net_unix_view_t *v);
int  a20_net_unix_enqueue(void *dst, const uint8_t *buf, size_t len,
                          const uint8_t *addr, size_t addrlen);
int  a20_net_unix_enqueue_fds(void *dst, const uint8_t *buf, size_t len,
                              const uint8_t *addr, size_t addrlen,
                              void **files, int nfiles);
void a20_net_unix_wakeq_init(void *wake_q);
void a20_net_unix_wakeq_flush(void *wake_q);

/*
 * Entry points the C wrappers call.  `max_path` is the caller's MAX_PATH_LEN
 * (a per-profile constant the Rust side must not assume) and `max_queue` is the
 * caller's NET_MAX_QUEUE, which bounds the accept backlog the same way it does
 * in C.
 */
int a20_net_unix_sockaddr_prepare(const void *addr, size_t addrlen,
                                  uint8_t *out, size_t *outlen,
                                  char *path_out, size_t path_outsz,
                                  size_t max_path);
int a20_net_unix_path_parent_ok(const char *path, size_t max_path);
int a20_net_unix_socket_bind(void *s, const void *addr, size_t addrlen,
                             size_t max_path);
int a20_net_unix_socket_connect(void *s, const void *addr, size_t addrlen,
                                size_t max_path, int max_queue);
int a20_net_unix_socket_sendto(void *s, const void *buf, size_t len,
                               const void *addr, size_t addrlen,
                               void **files, int nfiles, size_t max_path);
int a20_net_unix_ch_send(void *s, void *dst, const void *buf, size_t len);
int a20_net_unix_ch_recv(void *s, void *buf, size_t len);
int a20_net_unix_ch_peek(void *s, void *buf, size_t len);

#endif /* _NET_SOCKET_UNIX_STATE_H */
