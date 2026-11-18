#include "net/socket/socket_internal.h"
#include "net/tun.h"
#include "net/socket_file_state.h"
#include "proc/userns.h"
#include "ipc/ipc.h"
#include "net/stack.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/string.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/readiness.h"
#include "mm/slab.h"
#include "sys/usercopy.h"


static int net_vfile_read(vfile_t *vf, char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    /* read(2) on a socket is a receive with no flags, no address and no
     * metadata, which is exactly what the Rust receive transaction already
     * is: the algorithm dispatch, the channel path and the queue path with
     * its coalescing, refusal ordering, park and accounting all live there.
     * Keeping a second copy of that state machine here only risked the two
     * drifting apart. */
    return net_recvfrom_socket_meta(s, buf, count, 0, NULL, NULL, NULL);
}

static int net_vfile_write(vfile_t *vf, const char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    /* Linux netlink tools also use write(2), with the kernel as the implicit
     * destination. Share sendto's protocol dispatch and request validation. */
    if (s->domain == AF_NETLINK)
        return net_sendto_sock(s, (void *)buf, count, 0, NULL, 0);
    if (count > NET_MAX_PAYLOAD && s->type != SOCK_STREAM)
        return -EMSGSIZE;
    if (s->domain == AF_ALG)
        return net_alg_socket_send(s, buf, count);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        net_inet_has_transport(s))
        return net_inet_sendto(s, buf, count, 0, NULL, 0);
    /* Same rule and same reason as net_sendto_sock(): a stream socket in an
     * inet family with no pcb is a connection lwIP tore down, not a
     * non-socket, so it owes the caller EPIPE rather than ENOTSOCK. */
    if (s->type == SOCK_STREAM && !s->local_tcp && !net_inet_has_tcp(s) &&
        (s->domain == AF_INET || s->domain == AF_INET6))
        return s->ever_connected ? -EPIPE
                                  : net_notconn(NET_NOTCONN_VFS_WRITE_NO_PCB);
    if (s->domain == AF_UNIX)
        return net_unix_socket_sendto(s, buf, count, NULL, 0);

    /* Destination first, outside any lock: it comes from a whole-table scan that
     * walks the registry a bucket at a time, and the scan hands back a
     * reference, which is what lets this function then take s's and dst's own
     * locks without nesting a third one. */
    uint8_t src_local[NET_SOCKADDR_MAX];
    size_t src_local_len = 0;
    uint8_t peer_addr[NET_SOCKADDR_MAX];
    size_t peer_len = 0;
    bool connected = false;
    bool nonblock = false;
    uint64_t send_timeout = 0;
    net_socket_t *dst = NULL;
    {
        uint64_t sf = net_sock_lock(s);
        if (!net_socket_is_live(s)) {
            net_sock_unlock(s, sf);
            return -ENOTSOCK;
        }
        if (s->closed || s->shut_wr) {
            net_sock_unlock(s, sf);
            return -EPIPE;
        }
        if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
            net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
        src_local_len = s->local_len;
        memcpy(src_local, s->local, src_local_len);
        /* The peer pointer is sampled under s's lock, which is what keeps it
         * from being freed between the test and the net_socket_ref() below. */
        net_socket_t *peer = s->peer;
        if (peer && s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET &&
            !net_socket_is_live(peer))
            s->peer = NULL;
        dst = s->peer ? net_socket_ref(s->peer) : NULL;
        /* Everything below runs with no lock on s, so s's own answers are
         * copied out here rather than re-read. */
        connected = s->connected;
        nonblock = s->nonblock;
        send_timeout = s->send_timeout_ticks;
        peer_len = s->peer_len;
        if (peer_len)
            memcpy(peer_addr, s->peer_addr, peer_len);
        net_sock_unlock(s, sf);
    }
    if (!dst && connected)
        dst = net_find_bound_socket(s->net_ns, s->domain, s->type, peer_addr, peer_len);
    if (!dst)
        return connected ? -ECONNREFUSED : -EDESTADDRREQ;

    if (count <= NET_MAX_PAYLOAD) {
        int r;
        net_sock_pair_t pair = net_sock_lock2(s, dst);
        if (!net_socket_is_live(dst)) {
            net_sock_unlock2(pair);
            net_socket_free(dst);
            return connected ? -ECONNREFUSED : -EDESTADDRREQ;
        }
        if (dst->rx_count >= NET_MAX_QUEUE && !nonblock) {
            net_sock_unlock2(pair);
            r = net_enqueue_msg_blocking(s, dst, buf, count, src_local,
                                         src_local_len, nonblock, send_timeout);
            net_socket_free(dst);
            return r;
        }
        r = net_enqueue_msg_locked(dst, buf, count, src_local, src_local_len);
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        if (r >= 0)
            (void)wait_queue_collect_one(
                &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
        net_sock_unlock2(pair);
        (void)proc_wake_q_flush(&wake_q);
        net_socket_free(dst);
        return r;
    }

    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > NET_MAX_PAYLOAD)
            chunk = NET_MAX_PAYLOAD;
        int r = net_enqueue_msg_blocking(s, dst, buf + total, chunk,
                                         src_local, src_local_len,
                                         nonblock, send_timeout);
        if (r < 0) {
            net_socket_free(dst);
            return total ? (int)total : r;
        }
        total += (size_t)r;
    }
    net_socket_free(dst);
    return (int)total;
}

static long net_vfile_lseek(vfile_t *vf, long offset, int whence) {
    (void)vf; (void)offset; (void)whence;
    return -ESPIPE;
}


static size_t net_vfile_poll_sources(vfile_t *vf, short events,
                                     readiness_source_t *sources, size_t max)
{
    net_socket_t *s = vf ? vf->priv : NULL;
    if (!s || !sources)
        return 0;
    size_t count = 0;
    if (((events & POLLIN) || !(events & (POLLIN | POLLOUT))) && count < max) {
        wait_queue_t *queue = s->listening ? &s->accept_waitq : &s->read_waitq;
        sources[count++] = (readiness_source_t){ queue, 0, 0 };
        if (s->ch_ep && count < max)
            sources[count++] = (readiness_source_t){
                &s->ch_ep->waiters, A20_CH_WAIT_RECV, 0
            };
    }
    if ((events & POLLOUT) && count < max) {
        sources[count++] = (readiness_source_t){ &s->write_waitq, 0, 0 };
        if (s->ch_ep && count < max)
            sources[count++] = (readiness_source_t){
                &s->ch_ep->waiters, A20_CH_WAIT_SEND, 0
            };
    }
    return count;
}

static int net_vfile_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    return net_inet_interface_ioctl(vf ? vf->priv : NULL, req, arg);
}

static vfile_ops_t g_net_ops = {
    .read = net_vfile_read,
    .write = net_vfile_write,
    .lseek = net_vfile_lseek,
    .ioctl = net_vfile_ioctl,
    .poll = net_poll_file,
    .poll_sources = net_vfile_poll_sources,
    .close = net_socket_close_file,
};

int net_is_socket_vfile(struct vfile *vf)
{
    return vf && vf->ops == &g_net_ops && vf->priv;
}

int net_socket_stat_vfile(struct vfile *vf, kstat_t *st)
{
    if (!net_is_socket_vfile(vf))
        return -EBADF;
    if (!st)
        return -EINVAL;
    net_socket_t *s = vf->priv;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFSOCK | 0777;
    st->st_ino = vf->identity;
    st->st_uid = s->stat_uid;
    st->st_gid = s->stat_gid;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}

net_socket_t *net_socket_from_vfile(vfile_t *vf) {
    return (vf && net_is_socket_vfile(vf) && vf->priv) ?
           (net_socket_t *)vf->priv : NULL;
}

net_socket_t *net_socket_from_file(int gfd) {
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return NULL;
    net_socket_t *s = (vf->ops == &g_net_ops && vf->priv) ?
                      (net_socket_t *)vf->priv : NULL;
    vfs_put_file_ref(gfd, vf);
    return s;
}

/* Resolve exactly the vfile observed under the current fd table lock, then
 * pin its socket before allowing the fd-table reference to be dropped. The
 * caller retains both references until the syscall is entirely finished, so
 * closing/reusing the numeric descriptor cannot run the socket close callback
 * while the syscall still uses the object. */


/*
 * Push an event to every event queue watching this socket's global fd
 * (Native ABI event source wiring, docs/zh/native-abi/09-… §5).  Cheap when no
 * Native process watches the socket.  Called from the net core; safe with a
 * net lock held (a20_event_notify takes its own IRQ-safe locks).
 */
void net_event_notify(net_socket_t *s, uint32_t event, uint64_t data0,
                      uint64_t data1)
{
    if (!s || !s->vf)
        return;
    a20_event_notify(s->vf, A20_OBJ_SOCKET, event,
                     data0, data1);
}

/*
 * Rust lifecycle.
 *
 * What is left here is the primitive half: the view, the socket lock and the
 * address-ordered pair, the lwIP endpoint destroy, the channel release, the
 * wait-queue collect/wake/flush, the receive-queue detach, the registry
 * unregister and the socket free, the vfile allocator and the fd-table install.
 * Which of those to call, in what order, and which references a teardown owes
 * are the Rust transaction's decisions.
 */

void a20_net_close_view_init(void *sp, net_close_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    memset(out, 0, sizeof(*out));
    if (!s)
        return;
    out->s = s;
    out->closed = &s->closed;
    out->peer_closed = &s->peer_closed;
    out->peer = (void **)&s->peer;
    out->sock_type = &s->type;
    out->accept_head = (void **)&s->accept_head;
    out->accept_tail = (void **)&s->accept_tail;
    out->accept_count = &s->accept_count;
    out->accept_next = (void **)&s->accept_next;
    out->refs = &s->refs;
    out->vf_priv = NULL; /* the caller that owns the vfile fills this in */
}

void a20_net_close_lock(const net_close_view_t *v, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)v->s);
}

void a20_net_close_unlock(const net_close_view_t *v, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)v->s, flags);
}

void a20_net_close_lock_pair(const net_close_view_t *a,
                             const net_close_view_t *b,
                             const net_close_view_t **lo,
                             const net_close_view_t **hi, uint64_t *flags)
{
    net_socket_t *sa = a ? (net_socket_t *)a->s : NULL;
    net_socket_t *sb = b ? (net_socket_t *)b->s : NULL;
    net_sock_pair_t p = net_sock_lock2(sa, sb);
    *lo = (a && p.lo == sa) ? a : b;
    *hi = (!p.hi) ? NULL : ((a && p.hi == sa) ? a : b);
    *flags = p.flags;
}

void a20_net_close_unlock_pair(const net_close_view_t *lo,
                               const net_close_view_t *hi, uint64_t flags)
{
    net_sock_pair_t p;
    p.lo = lo ? (net_socket_t *)lo->s : NULL;
    p.hi = hi ? (net_socket_t *)hi->s : NULL;
    p.flags = flags;
    net_sock_unlock2(p);
}

void a20_net_close_inet_destroy(const net_close_view_t *v)
{
    net_inet_socket_destroy((net_socket_t *)v->s);
}

void a20_net_close_channel_release(const net_close_view_t *v)
{
    net_socket_t *s = (net_socket_t *)v->s;
    if (s->ch_ep) {
        a20_channel_ep_release(s->ch_ep);
        s->ch_ep = NULL;
    }
}

void *a20_net_close_waitq(const net_close_view_t *v, int which)
{
    net_socket_t *s = (net_socket_t *)v->s;
    switch (which) {
    case 0: return &s->accept_waitq;
    case 1: return &s->read_waitq;
    default: return &s->write_waitq;
    }
}

int a20_net_close_collect(void *q, int reason, void *wake_q)
{
    return net_wait_queue_collect_all_locked(
               (wait_queue_t *)q, (proc_wake_reason_t)reason,
               (proc_wake_q_t *)wake_q) ? 1 : 0;
}

void a20_net_close_wake(void *q, int reason)
{
    (void)wait_queue_wake_all((wait_queue_t *)q, 0,
                              (proc_wake_reason_t)reason);
}

void a20_net_close_flush(void *wake_q)
{
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

void a20_net_close_wake_q_reinit(void *wake_q)
{
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void *a20_net_close_rxq_detach(const net_close_view_t *v)
{
    return net_rxq_detach_locked((net_socket_t *)v->s);
}

void a20_net_close_msg_free_chain(void *m)
{
    net_msg_t *msg = (net_msg_t *)m;
    while (msg) {
        net_msg_t *next = msg->next;
        net_msg_free(msg);
        msg = next;
    }
}

int a20_net_close_unregister(const net_close_view_t *v)
{
    return net_socket_unregister((net_socket_t *)v->s) ? 1 : 0;
}

void a20_net_close_socket_free(const net_close_view_t *v)
{
    net_socket_free((net_socket_t *)v->s);
}

void *a20_net_install_prepare(void *sp, int flags)
{
    net_socket_t *s = (net_socket_t *)sp;
    vfile_t *vf = vfile_alloc();
    if (!vf)
        return NULL;
    vf->flags = flags & ~O_CLOEXEC;
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_net_ops;
    vf->priv = s;
    task_t *creator = proc_current();
    s->stat_uid = creator ? (uint32_t)creator->cred.fsuid : 0;
    s->stat_gid = creator ? (uint32_t)creator->cred.fsgid : 0;
    return vf;
}

int a20_net_install_fdtable(void *vf, int flags)
{
    return fdtable_install_current_vfile((vfile_t *)vf, flags);
}

void a20_net_install_set_owner(void *sp, int gfd, void *vf)
{
    net_socket_t *s = (net_socket_t *)sp;
    s->gfd = gfd;
    s->vf = (struct vfile *)vf;
}

void a20_net_install_free(void *vf)
{
    vfile_free((vfile_t *)vf);
}

void *a20_net_fdtable_current_file(int fd)
{
    return fdtable_get_current_file_ref(fd);
}

void *a20_net_vfile_socket(void *vf)
{
    return net_socket_from_vfile((vfile_t *)vf);
}

void a20_net_vfile_put(void *vf)
{
    vfs_put_file((vfile_t *)vf);
}

void a20_net_socket_ref(void *sp)
{
    (void)net_socket_ref((net_socket_t *)sp);
}

int net_socket_close_file(vfile_t *vf)
{
    proc_wake_q_t wake_q;
    net_close_view_t v;
    if (!vf)
        return 0;
    proc_wake_q_init(&wake_q);
    a20_net_close_view_init(vf->priv, &v);
    v.vf_priv = &vf->priv;
    return a20_net_socket_close(&v, &wake_q);
}

int net_socket_install_file(net_socket_t *s, int flags)
{
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    return a20_net_socket_install(s, flags, &wake_q);
}

int net_socket_pin_current_fd(int local_fd, vfile_t **out_vf,
                              net_socket_t **out_sock)
{
    return a20_net_socket_pin_current_fd(local_fd, (void **)out_vf,
                                         (void **)out_sock);
}

