#include "net/socket_internal.h"
#include "ipc/ipc.h"
#include "net/lwip_stack.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/string.h"
#include "fs/file.h"
#include "fs/readiness.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

static int net_vfile_read(vfile_t *vf, char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG)
        return net_alg_socket_recv(s, buf, count);
    if (s->ch_ep)
        return net_recvfrom_socket_meta(s, buf, count, 0, NULL, NULL, NULL);
    uint64_t start = timer_get_ticks();
    for (;;) {
        a20_lwip_poll();
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        int rx_count_before = s->rx_count;
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
        if (r != -EAGAIN || s->nonblock || s->closed || s->peer_closed || s->shut_rd) {
            if (r == -EAGAIN && (s->closed || s->peer_closed || s->shut_rd))
                r = 0;
            int recved = (r > 0 && s->type == SOCK_STREAM) ? r : 0;
            if (r > 0 && s->rx_count < rx_count_before)
                (void)wait_queue_collect_one(
                    &s->write_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            if (r > 0 && s->rx_head)
                (void)wait_queue_collect_one(
                    &s->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            spin_unlock_irqrestore(&g_net_lock, irq);
            (void)proc_wake_q_flush(&wake_q);
            if (recved)
                net_tcp_recved(s, (size_t)recved);
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
        uint64_t deadline = s->recv_timeout_ticks ?
                            start + s->recv_timeout_ticks : 0;
        spin_unlock_irqrestore(&g_net_lock, irq);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        irq = spin_lock_irqsave(&g_net_lock);
        if (s->rx_head || s->nonblock || s->closed || s->peer_closed ||
            s->shut_rd) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        if (net_task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ERESTARTSYS;
        }
        bool linked = wait_queue_link(&s->read_waitq, &entry, token, 0);
        spin_unlock_irqrestore(&g_net_lock, irq);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&s->read_waitq, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
        if (reason == PROC_WAKE_TIMEOUT)
            return -EAGAIN;
    }
}

static int net_vfile_write(vfile_t *vf, const char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    if (count > NET_MAX_PAYLOAD && s->type != SOCK_STREAM)
        return -EMSGSIZE;
    if (s->domain == AF_ALG)
        return net_alg_socket_send(s, buf, count);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        (s->udp || s->raw || s->tcp))
        return net_inet_sendto(s, buf, count, 0, NULL, 0);
    if (s->domain == AF_UNIX)
        return net_unix_socket_sendto(s, buf, count, NULL, 0);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->closed || s->shut_wr) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return -EPIPE;
    }
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    net_socket_t *dst = s->peer;
    if (dst && s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET && !net_socket_is_valid_locked(dst)) {
        s->peer = NULL;
        dst = NULL;
    }
    if (!dst && s->connected)
        dst = net_find_bound_socket_locked(s->domain, s->type, s->peer_addr, s->peer_len);
    if (!dst) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return s->connected ? -ECONNREFUSED : -EDESTADDRREQ;
    }

    if (count <= NET_MAX_PAYLOAD) {
        if (dst->rx_count >= NET_MAX_QUEUE && !s->nonblock) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return net_enqueue_msg_blocking(s, dst, buf, count, s->local, s->local_len,
                                            s->nonblock, s->send_timeout_ticks);
        }
        int r = net_enqueue_msg_locked(dst, buf, count, s->local, s->local_len);
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        if (r >= 0)
            (void)wait_queue_collect_one(
                &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
        spin_unlock_irqrestore(&g_net_lock, irq);
        (void)proc_wake_q_flush(&wake_q);
        return r;
    }

    spin_unlock_irqrestore(&g_net_lock, irq);

    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > NET_MAX_PAYLOAD)
            chunk = NET_MAX_PAYLOAD;
        int r = net_enqueue_msg_blocking(s, dst, buf + total, chunk,
                                          s->local, s->local_len,
                                          s->nonblock, s->send_timeout_ticks);
        if (r < 0)
            return total ? (int)total : r;
        total += (size_t)r;
    }
    return (int)total;
}

static long net_vfile_lseek(vfile_t *vf, long offset, int whence) {
    (void)vf; (void)offset; (void)whence;
    return -ESPIPE;
}

int net_socket_close_file(vfile_t *vf) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return 0;
    ktrace_net("[NET] close: dom=%d type=%d listen=%d tcp=%p peer=%p closed=%d\n",
               s->domain, s->type, s->listening, (void *)s->tcp,
               (void *)s->peer, s->closed);
    net_inet_socket_destroy(s);
    if (s->ch_ep) {
        a20_channel_ep_release(s->ch_ep);
        s->ch_ep = NULL;
    }
    ktrace_net("[NET] close: pcb dropped, unregistering\n");
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    net_socket_t *peer = NULL;
    bool drain_accept = false;
    bool drain_read = false;
    bool drain_write = false;
    bool drain_peer_read = false;
    bool drain_peer_write = false;
    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    net_unregister_socket_locked(s);
    drain_accept = net_wait_queue_collect_all_locked(
        &s->accept_waitq, PROC_WAKE_EXIT, &wake_q);
    drain_read = net_wait_queue_collect_all_locked(
        &s->read_waitq, PROC_WAKE_EXIT, &wake_q);
    drain_write = net_wait_queue_collect_all_locked(
        &s->write_waitq, PROC_WAKE_EXIT, &wake_q);
    if (s->peer && (s->type == SOCK_STREAM || s->type == SOCK_SEQPACKET || net_socket_is_valid_locked(s->peer)) && s->peer->peer == s) {
        peer = s->peer;
        peer->peer = NULL;
        peer->peer_closed = 1;
        drain_peer_read = net_wait_queue_collect_all_locked(
            &peer->read_waitq, PROC_WAKE_EVENT, &wake_q);
        drain_peer_write = net_wait_queue_collect_all_locked(
            &peer->write_waitq, PROC_WAKE_EVENT, &wake_q);
    }
    s->closed = 1;
    net_msg_t *m = s->rx_head;
#if CONFIG_DEBUG_NET_TRACE
    int rx_count = s->rx_count;
#endif
    s->rx_head = s->rx_tail = NULL;
    net_socket_t *accepted = s->accept_head;
#if CONFIG_DEBUG_NET_TRACE
    int acc_count = s->accept_count;
#endif
    s->accept_head = s->accept_tail = NULL;
    s->accept_count = 0;
    spin_unlock_irqrestore(&g_net_lock, flags);
    (void)proc_wake_q_flush(&wake_q);
    if (drain_accept)
        (void)wait_queue_wake_all(
            &s->accept_waitq, 0, PROC_WAKE_EXIT);
    if (drain_read)
        (void)wait_queue_wake_all(
            &s->read_waitq, 0, PROC_WAKE_EXIT);
    if (drain_write)
        (void)wait_queue_wake_all(
            &s->write_waitq, 0, PROC_WAKE_EXIT);
    if (drain_peer_read)
        (void)wait_queue_wake_all(
            &peer->read_waitq, 0, PROC_WAKE_EVENT);
    if (drain_peer_write)
        (void)wait_queue_wake_all(
            &peer->write_waitq, 0, PROC_WAKE_EVENT);
    ktrace_net("[NET] close: unregistered, rx=%d accept_q=%d\n", rx_count, acc_count);

    while (m) {
        net_msg_t *next = m->next;
        net_msg_free(m);
        m = next;
    }
    while (accepted) {
        net_socket_t *next = accepted->accept_next;
        proc_wake_q_init(&wake_q);
        peer = NULL;
        drain_read = false;
        drain_write = false;
        drain_peer_read = false;
        drain_peer_write = false;
        uint64_t accepted_flags = spin_lock_irqsave(&g_net_lock);
        accepted->closed = 1;
        if (accepted->peer && accepted->peer->peer == accepted) {
            peer = accepted->peer;
            peer->peer = NULL;
            peer->peer_closed = 1;
            drain_peer_read = net_wait_queue_collect_all_locked(
                &peer->read_waitq, PROC_WAKE_EVENT, &wake_q);
            drain_peer_write = net_wait_queue_collect_all_locked(
                &peer->write_waitq, PROC_WAKE_EVENT, &wake_q);
        }
        drain_read = net_wait_queue_collect_all_locked(
            &accepted->read_waitq, PROC_WAKE_EXIT, &wake_q);
        drain_write = net_wait_queue_collect_all_locked(
            &accepted->write_waitq, PROC_WAKE_EXIT, &wake_q);
        spin_unlock_irqrestore(&g_net_lock, accepted_flags);
        (void)proc_wake_q_flush(&wake_q);
        if (drain_read)
            (void)wait_queue_wake_all(
                &accepted->read_waitq, 0, PROC_WAKE_EXIT);
        if (drain_write)
            (void)wait_queue_wake_all(
                &accepted->write_waitq, 0, PROC_WAKE_EXIT);
        if (drain_peer_read)
            (void)wait_queue_wake_all(
                &peer->read_waitq, 0, PROC_WAKE_EVENT);
        if (drain_peer_write)
            (void)wait_queue_wake_all(
                &peer->write_waitq, 0, PROC_WAKE_EVENT);
        net_inet_socket_destroy(accepted);
        uint64_t unregister_flags = spin_lock_irqsave(&g_net_lock);
        net_unregister_socket_locked(accepted);
        spin_unlock_irqrestore(&g_net_lock, unregister_flags);
        net_socket_free(accepted);
        accepted = next;
    }
    net_socket_free(s);
    vf->priv = NULL;
    return 0;
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

/*
 * Interface ioctls (SIOCGIF*).  Standard network tooling -- udhcpc,
 * ifconfig, busybox `ip` -- configures an interface through these; with no
 * ioctl op at all the very first SIOCGIFHWADDR returned ENOTTY and the
 * interface could never be brought up.  Values match Linux/musl
 * <sys/ioctl.h>.  A struct ifreq is 16-byte name + a 16-byte union; the
 * address ioctls return a `struct sockaddr` (family + packed bytes).
 */
#define A20_SIOCGIFNAME     0x8910
#define A20_SIOCGIFCONF     0x8912
#define A20_SIOCGIFINDEX    0x8933
#define A20_SIOCGIFFLAGS    0x8913
#define A20_SIOCSIFFLAGS    0x8914
#define A20_SIOCGIFADDR     0x8915
#define A20_SIOCSIFADDR     0x8916
#define A20_SIOCGIFDSTADDR  0x8917
#define A20_SIOCSIFDSTADDR  0x8918
#define A20_SIOCGIFBRDADDR  0x8919
#define A20_SIOCSIFBRDADDR  0x891a
#define A20_SIOCGIFNETMASK  0x891b
#define A20_SIOCSIFNETMASK  0x891c
#define A20_SIOCGIFMTU      0x8921
#define A20_SIOCSIFMTU      0x8922
#define A20_SIOCGIFHWADDR   0x8927

#define A20_IFNAMSIZ        16
#define A20_IFF_UP          0x1
#define A20_IFF_BROADCAST   0x2
#define A20_IFF_LOOPBACK    0x8
#define A20_IFF_RUNNING     0x40
#define A20_ARPHRD_ETHER    1
#define A20_ARPHRD_LOOPBACK 772
#define A20_AF_INET         2

struct a20_ifreq {
    char ifr_name[A20_IFNAMSIZ];
    union {
        uint8_t raw[16];
        struct { uint16_t sa_family; uint8_t sa_data[14]; } addr;
        short flags;
        int ivalue;
    } ifr_ifru;
};

static struct netif *net_ifreq_lookup(const char *name)
{
    if (!name[0])
        return netif_default;
    struct netif *n = netif_find(name);
    if (n)
        return n;
    /* A20OS names its lwIP netif "en0" while the device node users see is
     * "net0"/"eth0"; accept those spellings as the default interface. */
    if (!strncmp(name, "en", 2) || !strncmp(name, "eth", 3) ||
        !strncmp(name, "net", 3))
        return netif_default;
    return NULL;
}

static int net_ifreq_put_addr(void *uarg, uint16_t family,
                              const void *bytes, size_t n)
{
    uint8_t out[16];
    memset(out, 0, sizeof(out));
    out[0] = (uint8_t)family;
    out[1] = (uint8_t)(family >> 8);
    if (n > 14)
        n = 14;
    memcpy(out + 2, bytes, n);
    return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, out, sizeof(out)) < 0
           ? -EFAULT : 0;
}

static int net_vfile_ifreq_ioctl(void *uarg, unsigned long req)
{
    struct a20_ifreq ifr;
    if (copy_from_user(&ifr, uarg, sizeof(ifr)) < 0)
        return -EFAULT;

    struct netif *nif = net_ifreq_lookup(ifr.ifr_name);
    if (!nif)
        return -ENODEV;

    switch (req) {
    case A20_SIOCGIFHWADDR:
        return net_ifreq_put_addr(uarg, A20_ARPHRD_ETHER,
                                  nif->hwaddr, nif->hwaddr_len);
    case A20_SIOCGIFADDR:
        return net_ifreq_put_addr(uarg, A20_AF_INET, &nif->ip_addr, 4);
    case A20_SIOCGIFNETMASK:
        return net_ifreq_put_addr(uarg, A20_AF_INET, &nif->netmask, 4);
    case A20_SIOCGIFDSTADDR:
    case A20_SIOCGIFBRDADDR: {
        uint32_t ip = 0, mask = 0, brd;
        memcpy(&ip, &nif->ip_addr, 4);
        memcpy(&mask, &nif->netmask, 4);
        brd = ip | ~mask;
        return net_ifreq_put_addr(uarg, A20_AF_INET, &brd, 4);
    }
    case A20_SIOCGIFMTU: {
        int mtu = (int)nif->mtu;
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &mtu, sizeof(mtu)) < 0
               ? -EFAULT : 0;
    }
    case A20_SIOCGIFINDEX: {
        int idx = (int)netif_name_to_index(nif->name);
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &idx, sizeof(idx)) < 0
               ? -EFAULT : 0;
    }
    case A20_SIOCGIFNAME:
        return 0;
    case A20_SIOCGIFFLAGS: {
        short fl = A20_IFF_BROADCAST;
        if (nif->flags & NETIF_FLAG_UP)
            fl |= A20_IFF_UP;
        if (nif->flags & NETIF_FLAG_LINK_UP)
            fl |= A20_IFF_RUNNING;
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &fl, sizeof(fl)) < 0
               ? -EFAULT : 0;
    }
    case A20_SIOCSIFFLAGS: {
        short fl = 0;
        if (copy_from_user(&fl, (uint8_t *)uarg + A20_IFNAMSIZ, sizeof(fl)) < 0)
            return -EFAULT;
        if (fl & A20_IFF_UP)
            netif_set_up(nif);
        else
            netif_set_down(nif);
        return 0;
    }
    case A20_SIOCSIFADDR: {
        uint8_t a[16];
        ip4_addr_t ip;
        if (copy_from_user(a, (uint8_t *)uarg + A20_IFNAMSIZ, sizeof(a)) < 0)
            return -EFAULT;
        memcpy(&ip, a + 2, 4);
        netif_set_ipaddr(nif, &ip);
        return 0;
    }
    case A20_SIOCSIFNETMASK: {
        uint8_t a[16];
        ip4_addr_t nm;
        if (copy_from_user(a, (uint8_t *)uarg + A20_IFNAMSIZ, sizeof(a)) < 0)
            return -EFAULT;
        memcpy(&nm, a + 2, 4);
        netif_set_netmask(nif, &nm);
        return 0;
    }
    }
    return -ENOTTY;
}

static int net_vfile_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    (void)vf;
    switch (req) {
    case A20_SIOCGIFNAME: case A20_SIOCGIFCONF: case A20_SIOCGIFFLAGS:
    case A20_SIOCSIFFLAGS: case A20_SIOCGIFADDR: case A20_SIOCSIFADDR:
    case A20_SIOCGIFDSTADDR: case A20_SIOCSIFDSTADDR: case A20_SIOCGIFBRDADDR:
    case A20_SIOCSIFBRDADDR: case A20_SIOCGIFNETMASK: case A20_SIOCSIFNETMASK:
    case A20_SIOCGIFMTU: case A20_SIOCSIFMTU: case A20_SIOCGIFHWADDR:
    case A20_SIOCGIFINDEX:
        return net_vfile_ifreq_ioctl(arg, req);
    }
    return -ENOTTY;
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
    s->gfd = gfd;
    return gfd;
}

/*
 * Push an event to every event queue watching this socket's global fd
 * (Native ABI event source wiring, docs/native-abi/09-… §5).  Cheap when no
 * Native process watches the socket.  Called from the net core; safe with
 * g_net_lock held (a20_event_notify takes its own IRQ-safe locks).
 */
void net_event_notify(net_socket_t *s, uint32_t event, uint64_t data0,
                      uint64_t data1)
{
    if (!s || s->gfd < 0)
        return;
    a20_event_notify((void *)(uintptr_t)s->gfd, A20_OBJ_SOCKET, event,
                     data0, data1);
}
