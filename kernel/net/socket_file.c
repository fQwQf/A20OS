#include "net/socket_internal.h"
#include "net/lwip_stack.h"
#include "core/consts.h"
#include "core/string.h"
#include "fs/file.h"
#include "mm/slab.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"

static int net_vfile_read(vfile_t *vf, char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG)
        return net_alg_socket_recv(s, buf, count);
    uint64_t start = timer_get_ticks();
    for (;;) {
        a20_lwip_poll();
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        int r = net_dequeue_msg_locked(s, buf, count, NULL, NULL);
        if (r > 0 && s->type == SOCK_STREAM) {
            size_t total = (size_t)r;
            while (total < count && s->rx_head) {
                int nr = net_dequeue_msg_locked(s, buf + total,
                                                count - total, NULL, NULL);
                if (nr <= 0)
                    break;
                total += (size_t)nr;
            }
            r = (int)total;
        }
        if (r != -EAGAIN || s->nonblock) {
            spin_unlock_irqrestore(&g_net_lock, irq);
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
        if (net_socket_wait_expired(s, start, 0)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EAGAIN;
        }
        net_block_on_socket_locked(s, cur);
        spin_unlock_irqrestore(&g_net_lock, irq);
        sched();
        net_clear_socket_waiter(s, cur);
    }
}

static int net_vfile_write(vfile_t *vf, const char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    if (count > NET_MAX_PAYLOAD)
        return -EMSGSIZE;
    if (s->domain == AF_ALG)
        return net_alg_socket_send(s, buf, count);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        (s->udp || s->raw || s->tcp))
        return net_inet_sendto(s, buf, count, 0, NULL, 0);
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    net_socket_t *dst = s->peer;
    if (!dst && s->connected)
        dst = net_find_bound_socket_locked(s->domain, s->type, s->peer_addr, s->peer_len);
    if (!dst) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return s->connected ? -ECONNREFUSED : -EDESTADDRREQ;
    }
    if (dst->rx_count >= NET_MAX_QUEUE && !s->nonblock) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return net_enqueue_msg_blocking(dst, buf, count, s->local, s->local_len,
                                        0, s->send_timeout_ticks);
    }
    int r = net_enqueue_msg_locked(dst, buf, count, s->local, s->local_len);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return r;
}

static long net_vfile_lseek(vfile_t *vf, long offset, int whence) {
    (void)vf; (void)offset; (void)whence;
    return -ESPIPE;
}

int net_socket_close_file(vfile_t *vf) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return 0;
    if (s->udp) {
        udp_remove(s->udp);
        s->udp = NULL;
    }
    if (s->raw) {
        raw_remove(s->raw);
        s->raw = NULL;
    }
    net_tcp_close_pcb(s);
    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    net_unregister_socket_locked(s);
    if (s->peer && s->peer->peer == s) {
        s->peer->peer = NULL;
        s->peer->closed = 1;
        if (s->peer->waiter && s->peer->waiter->state == PROC_BLOCKED)
            proc_make_ready(s->peer->waiter);
    }
    s->closed = 1;
    if (s->waiter && s->waiter->state == PROC_BLOCKED)
        proc_make_ready(s->waiter);
    net_msg_t *m = s->rx_head;
    s->rx_head = s->rx_tail = NULL;
    net_socket_t *accepted = s->accept_head;
    s->accept_head = s->accept_tail = NULL;
    s->accept_count = 0;
    spin_unlock_irqrestore(&g_net_lock, flags);

    while (m) {
        net_msg_t *next = m->next;
        net_msg_free(m);
        m = next;
    }
    while (accepted) {
        net_socket_t *next = accepted->accept_next;
        net_tcp_close_pcb(accepted);
        net_socket_free(accepted);
        accepted = next;
    }
    net_socket_free(s);
    vf->priv = NULL;
    return 0;
}

static vfile_ops_t g_net_ops = {
    .read = net_vfile_read,
    .write = net_vfile_write,
    .lseek = net_vfile_lseek,
    .close = net_socket_close_file,
};

net_socket_t *net_socket_from_file(int gfd) {
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return NULL;
    net_socket_t *s = (vf->ops == &g_net_ops && vf->priv) ?
                      (net_socket_t *)vf->priv : NULL;
    vfs_put_file_ref(gfd, vf);
    return s;
}

int net_socket_install_file(net_socket_t *s, int flags) {
    vfile_t *vf = vfile_alloc();
    if (!vf)
        return -ENOMEM;
    vf->flags = flags;
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_net_ops;
    vf->priv = s;

    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        net_socket_close_file(vf);
        vfile_free(vf);
        return gfd;
    }
    return gfd;
}
