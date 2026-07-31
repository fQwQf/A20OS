#include "net/socket_internal.h"
#include "fs/file.h"
#include "mm/objcache.h"
#include "sys/bpf.h"
#include "core/string.h"
#include "core/timer.h"
#include "proc/proc.h"

static obj_cache_t g_net_msg_cache = OBJ_CACHE_INIT("net_msg", net_msg_t, 16);

net_msg_t *net_msg_alloc(void)
{
    return (net_msg_t *)obj_cache_alloc_zero(&g_net_msg_cache);
}

void net_msg_free(net_msg_t *m)
{
    if (!m)
        return;
    for (int i = 0; i < m->scm_nfiles; i++)
        vfs_put_file(m->scm_files[i]);
    m->scm_nfiles = 0;
    obj_cache_free(&g_net_msg_cache, m);
}

void net_scm_drop_files(vfile_t **files, int nfiles)
{
    if (!files)
        return;
    for (int i = 0; i < nfiles; i++)
        vfs_put_file(files[i]);
}

int net_enqueue_msg_locked_meta(net_socket_t *dst, const void *buf, size_t len,
                                const void *addr, size_t addrlen,
                                const net_bh_event_t *meta)
{
    if (!dst || dst->closed)
        return -ENOTCONN;
    if (len > NET_MAX_PAYLOAD)
        return -EMSGSIZE;
    if (dst->rx_count >= NET_MAX_QUEUE)
        return -EAGAIN;
    if (dst->bpf_prog_fd >= 0)
        bpf_run_socket_filter(dst->bpf_prog_fd);

    net_msg_t *m = net_msg_alloc();
    if (!m)
        return -EAGAIN;
    memset(m, 0, sizeof(*m));
    memcpy(m->data, buf, len);
    m->len = len;
    m->off = 0;
    if (addr && addrlen) {
        if (addrlen > NET_SOCKADDR_MAX)
            addrlen = NET_SOCKADDR_MAX;
        memcpy(m->addr, addr, addrlen);
        m->addrlen = addrlen;
    }
    if (meta) {
        m->has_pktinfo = meta->has_pktinfo;
        m->has_hoplimit = meta->has_hoplimit;
        m->has_tclass = meta->has_tclass;
        m->pktinfo_ifindex = meta->pktinfo_ifindex;
        memcpy(m->pktinfo_addr, meta->pktinfo_addr, sizeof(m->pktinfo_addr));
        m->hoplimit = meta->hoplimit;
        m->tclass = meta->tclass;
    }
    if (dst->rx_tail)
        dst->rx_tail->next = m;
    else
        dst->rx_head = m;
    dst->rx_tail = m;
    dst->rx_count++;
    return (int)len;
}

int net_enqueue_msg_locked(net_socket_t *dst, const void *buf, size_t len,
                           const void *addr, size_t addrlen)
{
    return net_enqueue_msg_locked_meta(dst, buf, len, addr, addrlen, NULL);
}

int net_enqueue_msg_locked_fds(net_socket_t *dst, const void *buf,
                               size_t len, const void *addr,
                               size_t addrlen,
                               vfile_t **files, int nfiles)
{
    if (nfiles < 0 || nfiles > NET_SCM_MAX_FDS)
        return -EINVAL;
    int r = net_enqueue_msg_locked(dst, buf, len, addr, addrlen);
    if (r < 0)
        return r;
    if (nfiles > 0 && files) {
        net_msg_t *m = dst->rx_tail;
        m->scm_nfiles = nfiles;
        for (int i = 0; i < nfiles; i++)
            m->scm_files[i] = files[i];
    }
    return r;
}

int net_enqueue_msg_blocking(net_socket_t *s, net_socket_t *dst, const void *buf, size_t len,
                              const void *addr, size_t addrlen,
                              int dontwait, uint64_t timeout_ticks)
{
    uint64_t start = timer_get_ticks();
    for (;;) {
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        if (!net_socket_is_valid_locked(s) || s->closed) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -ENOTCONN;
        }
        if (!net_socket_is_valid_locked(dst) || dst->closed) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -ENOTCONN;
        }
        /* UDP connect sets peer_addr but NOT s->peer, so s->peer is
           legitimately NULL — skip this check for DGRAM. */
        if (s->connected && s->peer != dst &&
            s->type != SOCK_DGRAM) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -ENOTCONN;
        }
        int r = net_enqueue_msg_locked(dst, buf, len, addr, addrlen);
        if (r != -EAGAIN || dontwait) {
            if (r >= 0)
                (void)wait_queue_collect_one(
                    &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            spin_unlock_irqrestore(&g_net_lock, irq);
            (void)proc_wake_q_flush(&wake_q);
            return r;
        }
        task_t *cur = proc_current();
        if (!cur) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EAGAIN;
        }
        if (net_task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -ERESTARTSYS;
        }
        if (timeout_ticks &&
            (int64_t)(timer_get_ticks() - (start + timeout_ticks)) >= 0) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EAGAIN;
        }
        if (!timeout_ticks && s->type == SOCK_DGRAM) {
            uint64_t udp_deadline = start + MS_TO_TICKS(200);
            if ((int64_t)(timer_get_ticks() - udp_deadline) >= 0) {
                spin_unlock_irqrestore(&g_net_lock, irq);
                return -EAGAIN;
            }
        }
        if (!timeout_ticks && s->type == SOCK_STREAM) {
            uint64_t tcp_deadline = start + MS_TO_TICKS(5000);
            if ((int64_t)(timer_get_ticks() - tcp_deadline) >= 0) {
                spin_unlock_irqrestore(&g_net_lock, irq);
                return -EAGAIN;
            }
        }
        uint64_t deadline = timeout_ticks ? start + timeout_ticks : 0;
        if (!deadline && s->type == SOCK_DGRAM)
            deadline = start + MS_TO_TICKS(200);
        if (!deadline && s->type == SOCK_STREAM)
            deadline = start + MS_TO_TICKS(5000);
        spin_unlock_irqrestore(&g_net_lock, irq);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        irq = spin_lock_irqsave(&g_net_lock);
        if (!net_socket_is_valid_locked(s) || s->closed ||
            !net_socket_is_valid_locked(dst) || dst->closed ||
            (s->connected && s->peer != dst && s->type != SOCK_DGRAM)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ENOTCONN;
        }
        r = net_enqueue_msg_locked(dst, buf, len, addr, addrlen);
        if (r != -EAGAIN) {
            if (r >= 0)
                (void)wait_queue_collect_one(
                    &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            spin_unlock_irqrestore(&g_net_lock, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            (void)proc_wake_q_flush(&wake_q);
            return r;
        }
        if (net_task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ERESTARTSYS;
        }
        bool linked =
            wait_queue_link(&dst->write_waitq, &entry, token, 0);
        spin_unlock_irqrestore(&g_net_lock, irq);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&dst->write_waitq, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
        if (reason == PROC_WAKE_TIMEOUT)
            return -EAGAIN;
    }
}

int net_dequeue_msg_locked_meta(net_socket_t *s, void *buf, size_t len,
                                void *addr, size_t *addrlen,
                                net_recv_meta_t *meta)
{
    net_msg_t *m = s->rx_head;
    if (!m) {
        if (s->closed || s->peer_closed || s->shut_rd)
            return 0;
        return -EAGAIN;
    }
    size_t avail = m->len - m->off;
    size_t n = avail < len ? avail : len;
    memcpy(buf, m->data + m->off, n);
    if (addr && addrlen && *addrlen > 0) {
        size_t alen = m->addrlen < *addrlen ? m->addrlen : *addrlen;
        memcpy(addr, m->addr, alen);
        *addrlen = alen;
    }
    if (meta) {
        meta->has_pktinfo = m->has_pktinfo;
        meta->has_hoplimit = m->has_hoplimit;
        meta->has_tclass = m->has_tclass;
        meta->pktinfo_ifindex = m->pktinfo_ifindex;
        memcpy(meta->pktinfo_addr, m->pktinfo_addr, sizeof(meta->pktinfo_addr));
        meta->hoplimit = m->hoplimit;
        meta->tclass = m->tclass;
        /* SCM_RIGHTS fds attach to the first byte of the message: they
         * are delivered exactly once, with the first dequeue.  Appends to
         * any fds already collected from earlier messages in the same
         * recv; overflow stays attached and is dropped when the message
         * is freed. */
        if (m->off == 0 && m->scm_nfiles > 0) {
            for (int i = 0; i < m->scm_nfiles; i++) {
                if (meta->scm_nfiles < NET_SCM_MAX_FDS) {
                    meta->scm_files[meta->scm_nfiles++] = m->scm_files[i];
                    m->scm_files[i] = NULL;
                }
            }
            int left = 0;
            for (int i = 0; i < m->scm_nfiles; i++) {
                if (m->scm_files[i])
                    m->scm_files[left++] = m->scm_files[i];
            }
            m->scm_nfiles = left;
        }
    }

    if (s->type == SOCK_STREAM && n < avail) {
        m->off += n;
        return (int)n;
    }

    s->rx_head = m->next;
    if (!s->rx_head)
        s->rx_tail = NULL;
    s->rx_count--;
    net_msg_free(m);
    return (int)n;
}

int net_dequeue_msg_locked(net_socket_t *s, void *buf, size_t len,
                           void *addr, size_t *addrlen)
{
    return net_dequeue_msg_locked_meta(s, buf, len, addr, addrlen, NULL);
}

int net_accept_queue_push_locked(net_socket_t *listener, net_socket_t *child)
{
    if (!listener || !child || !listener->listening)
        return -EINVAL;
    if (listener->accept_count >= NET_MAX_QUEUE)
        return -EAGAIN;

    child->accept_next = NULL;
    if (listener->accept_tail)
        listener->accept_tail->accept_next = child;
    else
        listener->accept_head = child;
    listener->accept_tail = child;
    listener->accept_count++;
    return 0;
}

net_socket_t *net_accept_queue_pop_locked(net_socket_t *listener)
{
    if (!listener)
        return NULL;

    net_socket_t *child = listener->accept_head;
    if (!child)
        return NULL;

    listener->accept_head = child->accept_next;
    if (!listener->accept_head)
        listener->accept_tail = NULL;
    listener->accept_count--;
    child->accept_next = NULL;
    return child;
}
