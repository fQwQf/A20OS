#include "net/socket/socket_internal.h"
#include "ipc/ipc.h"
#include "fs/vfs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"

#include "net/socket_unix_state.h"

/*
 * The AF_UNIX state machine is Rust (kernel/net/socket/socket_unix.rs).  What is left
 * here is the primitive set it calls, and the wrappers that keep the C names
 * the rest of the tree uses.
 *
 * Rust owns the address preparation, the bind transaction and its rollback, the
 * connect transaction that allocates and wires the accepted child, the sendto
 * transaction with its destination resolution and stream fragmentation, and the
 * channel-backed data plane.  Everything below is a lock, a scheduler call, an
 * allocation, a vfs call, a channel call or a whole-table scan: the things that
 * cannot move without moving the lock discipline or the real I/O with them.
 *
 * The lock pair is handed back to Rust as an opaque (lo, hi, flags) triple so
 * the ascending-address ordering stays in one place -- net_sock_lock2() --
 * instead of being re-derived on the Rust side.
 */

void a20_net_unix_view_init(void *sp, net_unix_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->protocol = &s->protocol;
    out->nonblock = &s->nonblock;
    out->bound = &s->bound;
    out->connected = &s->connected;
    out->ever_connected = &s->ever_connected;
    out->listening = &s->listening;
    out->accept_count = &s->accept_count;
    out->passcred = &s->passcred;
    out->owner_pid = &s->owner_pid;
    out->owner_uid = &s->owner_uid;
    out->owner_gid = &s->owner_gid;
    out->peer_pid = &s->peer_pid;
    out->peer_uid = &s->peer_uid;
    out->peer_gid = &s->peer_gid;
    out->ch_cred_pid = &s->ch_cred_pid;
    out->ch_cred_uid = &s->ch_cred_uid;
    out->ch_cred_gid = &s->ch_cred_gid;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->peer_addr = s->peer_addr;
    out->peer_len = &s->peer_len;
    out->peer = (void **)&s->peer;
    out->accept_next = (void **)&s->accept_next;
    out->ch_ep = (void **)&s->ch_ep;
    out->ch_buf = &s->ch_buf;
    out->ch_len = &s->ch_len;
}

void a20_net_unix_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_unix_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

void a20_net_unix_lock2(void *a, void *b, net_unix_pair_t *out)
{
    net_sock_pair_t p = net_sock_lock2((net_socket_t *)a, (net_socket_t *)b);
    out->lo = p.lo;
    out->hi = p.hi;
    out->flags = p.flags;
}

void a20_net_unix_unlock2(net_unix_pair_t *in)
{
    net_sock_pair_t p;
    p.lo = in->lo;
    p.hi = in->hi;
    p.flags = in->flags;
    net_sock_unlock2(p);
}

int a20_net_unix_is_live(void *s)
{
    return net_socket_is_live((const net_socket_t *)s) ? 1 : 0;
}

void a20_net_unix_ref(void *s)
{
    net_socket_ref((net_socket_t *)s);
}

void a20_net_unix_free(void *s)
{
    net_socket_free((net_socket_t *)s);
}

void *a20_net_unix_alloc(void)
{
    return net_socket_alloc();
}

void *a20_net_unix_find_bound(void *ns, int domain, int sock_type,
                             const uint8_t *addr, size_t addrlen)
{
    return net_find_bound_socket((resource_namespace_t *)ns, domain, sock_type,
                                 addr, addrlen);
}

int a20_net_unix_creds(int32_t *pid, int32_t *uid, int32_t *gid)
{
    task_t *cur = proc_current();
    if (!cur)
        return -1;
    *pid = cur->tgid;
    *uid = (int32_t)cur->cred.uid;
    *gid = (int32_t)cur->cred.gid;
    return 0;
}

void a20_net_unix_cwd(char *out, size_t outsz)
{
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->fs.cwd[0]) ? cur->fs.cwd : "/";
    strncpy(out, cwd, outsz - 1);
    out[outsz - 1] = '\0';
}

int a20_net_unix_path_is_dir(const char *path)
{
    kstat_t st;
    int r = vfs_stat(path, &st);
    if (r < 0)
        return r;
    return ((st.st_mode & S_IFMT) == S_IFDIR) ? 0 : -ENOTDIR;
}

int a20_net_unix_path_stat(const char *path)
{
    kstat_t st;
    return vfs_stat(path, &st);
}

int a20_net_unix_path_create(const char *path)
{
    return vfs_open(path, O_CREAT | O_EXCL | O_RDWR, S_IFSOCK | 0777);
}

void a20_net_unix_path_close(int fd)
{
    vfs_close(fd);
}

int a20_net_unix_accept_push(void *listener, void *child)
{
    return net_accept_queue_push_locked((net_socket_t *)listener,
                                        (net_socket_t *)child);
}

int a20_net_unix_accept_collect(void *listener, void *wake_q)
{
    net_socket_t *l = (net_socket_t *)listener;
    int r = (int)wait_queue_collect_one(&l->accept_waitq, 0, PROC_WAKE_EVENT,
                                        (proc_wake_q_t *)wake_q);
    net_event_notify(l, A20_EVENT_ACCEPT_READY, 0, 0);
    return r;
}

int a20_net_unix_read_collect(void *s, void *wake_q)
{
    net_socket_t *sk = (net_socket_t *)s;
    net_event_notify(sk, A20_EVENT_READABLE, 0, 0);
    return (int)wait_queue_collect_one(&sk->read_waitq, 0, PROC_WAKE_EVENT,
                                       (proc_wake_q_t *)wake_q);
}

void a20_net_unix_read_wake_all(void *s)
{
    net_socket_t *sk = (net_socket_t *)s;
    net_event_notify(sk, A20_EVENT_READABLE, 0, 0);
    wait_queue_wake_all(&sk->read_waitq, 0, PROC_WAKE_EVENT);
}

void a20_net_unix_channel_release(const net_unix_view_t *v)
{
    net_socket_t *s = (net_socket_t *)v->s;
    if (s->ch_ep) {
        a20_channel_ep_release(s->ch_ep);
        s->ch_ep = NULL;
    }
}

int64_t a20_net_unix_channel_send(void *ep, const uint8_t *buf, uint32_t len,
                                  uint32_t flags)
{
    return a20_channel_send((a20_channel_ep_t *)ep, buf, len, NULL, 0, NULL,
                            flags);
}

int64_t a20_net_unix_channel_recv(void *ep, uint8_t *buf, uint32_t *cap)
{
    return a20_channel_recv((a20_channel_ep_t *)ep, buf, cap, NULL, NULL, NULL,
                            A20_MSG_NONBLOCK);
}

uint8_t *a20_net_unix_chbuf(const net_unix_view_t *v)
{
    net_socket_t *s = (net_socket_t *)v->s;
    if (!s->ch_buf) {
        s->ch_buf = kmalloc(A20_CH_MAX_DATA);
        if (!s->ch_buf)
            return NULL;
    }
    return (uint8_t *)s->ch_buf;
}

int a20_net_unix_enqueue(void *dst, const uint8_t *buf, size_t len,
                         const uint8_t *addr, size_t addrlen)
{
    return net_enqueue_msg_locked((net_socket_t *)dst, buf, len, addr, addrlen);
}

int a20_net_unix_enqueue_fds(void *dst, const uint8_t *buf, size_t len,
                             const uint8_t *addr, size_t addrlen,
                             void **files, int nfiles)
{
    return net_enqueue_msg_locked_fds((net_socket_t *)dst, buf, len, addr,
                                      addrlen, (vfile_t **)files, nfiles);
}

void a20_net_unix_wakeq_init(void *wake_q)
{
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void a20_net_unix_wakeq_flush(void *wake_q)
{
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

int net_unix_socket_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    return a20_net_unix_socket_bind(s, addr, addrlen, MAX_PATH_LEN);
}

int net_unix_socket_connect(net_socket_t *s, const void *addr, size_t addrlen)
{
    return a20_net_unix_socket_connect(s, addr, addrlen, MAX_PATH_LEN,
                                       NET_MAX_QUEUE);
}

int net_unix_socket_sendto(net_socket_t *s, const void *buf, size_t len,
                           const void *addr, size_t addrlen)
{
    return a20_net_unix_socket_sendto(s, buf, len, addr, addrlen, NULL, 0,
                                      MAX_PATH_LEN);
}

int net_unix_socket_sendto_fds(net_socket_t *s, const void *buf, size_t len,
                               const void *addr, size_t addrlen,
                               vfile_t **files, int nfiles)
{
    return a20_net_unix_socket_sendto(s, buf, len, addr, addrlen,
                                      (void **)files, nfiles, MAX_PATH_LEN);
}

int unix_ch_send(net_socket_t *s, net_socket_t *dst, const void *buf,
                 size_t len)
{
    return a20_net_unix_ch_send(s, dst, buf, len);
}

int unix_ch_recv(net_socket_t *s, void *buf, size_t len)
{
    return a20_net_unix_ch_recv(s, buf, len);
}

int unix_ch_peek(net_socket_t *s, void *buf, size_t len)
{
    return a20_net_unix_ch_peek(s, buf, len);
}
