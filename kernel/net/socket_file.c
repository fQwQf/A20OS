#include "net/socket_internal.h"
#include "ipc/ipc.h"
#include "net/lwip_stack.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/string.h"
#include "fs/file.h"
#include "fs/fdtable.h"
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
    int sb = net_socket_bucket(s);
    for (;;) {
        a20_lwip_poll_waiter();
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t irq = net_bucket_lock(sb);
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
            net_bucket_unlock(sb, irq);
            (void)proc_wake_q_flush(&wake_q);
            if (recved)
                net_tcp_recved(s, (size_t)recved);
            return r;
        }
        task_t *cur = proc_current();
        if (!cur) {
            net_bucket_unlock(sb, irq);
            return -EAGAIN;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_bucket_unlock(sb, irq);
            return -ERESTARTSYS;
        }
        if (net_socket_wait_expired(s, start, 0)) {
            net_bucket_unlock(sb, irq);
            return -EAGAIN;
        }
        uint64_t deadline = s->recv_timeout_ticks ?
                            start + s->recv_timeout_ticks : 0;
        net_bucket_unlock(sb, irq);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        irq = net_bucket_lock(sb);
        if (s->rx_head || s->nonblock || s->closed || s->peer_closed ||
            s->shut_rd) {
            net_bucket_unlock(sb, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_bucket_unlock(sb, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ERESTARTSYS;
        }
        bool linked = wait_queue_link(&s->read_waitq, &entry, token, 0);
        net_bucket_unlock(sb, irq);
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
    /* Same rule and same reason as net_sendto_sock(): a stream socket in an
     * inet family with no pcb is a connection lwIP tore down, not a
     * non-socket, so it owes the caller EPIPE rather than ENOTSOCK. */
    if (s->type == SOCK_STREAM && !s->local_tcp && !s->tcp &&
        (s->domain == AF_INET || s->domain == AF_INET6))
        return s->ever_connected ? -EPIPE : -ENOTCONN;
    if (s->domain == AF_UNIX)
        return net_unix_socket_sendto(s, buf, count, NULL, 0);

    /* Destination first, outside any lock: it comes from a whole-table scan
     * that has to take the shard locks one at a time, and the scan hands back a
     * reference, which is what lets this function then take s's and dst's
     * buckets in ascending order without nesting a third one. */
    int sb = net_socket_bucket(s);
    uint8_t src_local[NET_SOCKADDR_MAX];
    size_t src_local_len = 0;
    net_socket_t *dst = NULL;
    {
        uint64_t sf = net_bucket_lock(sb);
        if (!net_socket_is_live(s)) {
            net_bucket_unlock(sb, sf);
            return -ENOTSOCK;
        }
        if (s->closed || s->shut_wr) {
            net_bucket_unlock(sb, sf);
            return -EPIPE;
        }
        if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
            net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
        src_local_len = s->local_len;
        memcpy(src_local, s->local, src_local_len);
        if (s->peer && s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET &&
            !net_socket_is_live(s->peer))
            s->peer = NULL;
        dst = s->peer ? net_socket_ref(s->peer) : NULL;
        net_bucket_unlock(sb, sf);
    }
    if (!dst && s->connected)
        dst = net_find_bound_socket(s->domain, s->type, s->peer_addr,
                                    s->peer_len);
    if (!dst) {
        return s->connected ? -ECONNREFUSED : -EDESTADDRREQ;
    }

    if (count <= NET_MAX_PAYLOAD) {
        int r;
        net_bucket_pair_t pair = net_bucket_lock2(sb,
                                                  net_socket_bucket(dst));
        if (!net_socket_is_live(dst)) {
            net_bucket_unlock2(pair);
            net_socket_free(dst);
            return s->connected ? -ECONNREFUSED : -EDESTADDRREQ;
        }
        if (dst->rx_count >= NET_MAX_QUEUE && !s->nonblock) {
            net_bucket_unlock2(pair);
            r = net_enqueue_msg_blocking(s, dst, buf, count, src_local,
                                         src_local_len, s->nonblock,
                                         s->send_timeout_ticks);
            net_socket_free(dst);
            return r;
        }
        r = net_enqueue_msg_locked(dst, buf, count, src_local, src_local_len);
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        if (r >= 0)
            (void)wait_queue_collect_one(
                &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
        net_bucket_unlock2(pair);
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
                                         s->nonblock, s->send_timeout_ticks);
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
    /* s and its peer: two buckets, ascending.  s->peer is a plain back-pointer
     * with no reference of its own, so it is sampled under s's own bucket and
     * a reference taken there -- reading it before any lock could name a socket
     * another CPU has already freed.  A peer that was unregistered in the
     * window between the two steps lands in the orphan shard, which is what the
     * pair below then holds, and net_socket_is_live() rejects it. */
    int sb = net_socket_bucket(s);
    {
        uint64_t sf = net_bucket_lock(sb);
        if (s->peer)
            peer = net_socket_ref(s->peer);
        net_bucket_unlock(sb, sf);
    }
    net_bucket_pair_t pair = net_bucket_lock2(sb, net_socket_bucket(peer));
    net_unregister_socket_locked(s);
    drain_accept = net_wait_queue_collect_all_locked(
        &s->accept_waitq, PROC_WAKE_EXIT, &wake_q);
    drain_read = net_wait_queue_collect_all_locked(
        &s->read_waitq, PROC_WAKE_EXIT, &wake_q);
    drain_write = net_wait_queue_collect_all_locked(
        &s->write_waitq, PROC_WAKE_EXIT, &wake_q);
    if (peer &&
        (s->type == SOCK_STREAM || s->type == SOCK_SEQPACKET ||
         net_socket_is_live(peer)) && peer->peer == s) {
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
    net_bucket_unlock2(pair);
    /* Registry reference, dropped outside the lock because it can free. */
    net_socket_free(s);
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
    if (peer)
        net_socket_free(peer);
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
        /* Each accepted child is torn down on its own, with its own peer:
         * two buckets, ascending.  The children's accept list was detached
         * above, so no lock from the listener's teardown is still held and the
         * children need not all share a bucket.  The child's peer is sampled
         * and referenced under the child's own bucket, for the same reason as
         * above: the back-pointer carries no reference of its own. */
        int cb = net_socket_bucket(accepted);
        {
            uint64_t cf = net_bucket_lock(cb);
            if (accepted->peer)
                peer = net_socket_ref(accepted->peer);
            net_bucket_unlock(cb, cf);
        }
        net_bucket_pair_t apair = net_bucket_lock2(cb, net_socket_bucket(peer));
        accepted->closed = 1;
        if (peer && peer->peer == accepted) {
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
        net_unregister_socket_locked(accepted);
        net_bucket_unlock2(apair);
        net_socket_free(accepted);
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
        if (peer)
            net_socket_free(peer);
        net_inet_socket_destroy(accepted);
        /* Creator's reference, dropped last; the registry's was dropped above
         * once the slot was released. */
        net_socket_free(accepted);
        accepted = next;
    }
    /* Creator's reference for s, likewise. */
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
#define A20_AF_UNSPEC       0
#define A20_AF_INET         2

/* Linux's `struct ifmap`, which is what sizes the ifreq union.  It is included
 * because its width depends on `unsigned long`, exactly as upstream's does, so
 * the resulting ifreq size comes out right on both LP64 and ILP32.  Omitting it
 * leaves the union 16 bytes wide, the struct 8 bytes shorter than every real
 * libc's, and a caller walking the SIOCGIFCONF buffer in whole `struct ifreq`
 * strides desynchronises after the first entry. */
struct a20_ifmap {
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char  irq;
    unsigned char  dma;
    unsigned short port;
};

struct a20_ifreq {
    char ifr_name[A20_IFNAMSIZ];
    union {
        uint8_t raw[16];
        struct { uint16_t sa_family; uint16_t sa_port; uint32_t sa_addr; uint8_t sa_zero[8]; } addr;
        struct a20_ifmap map;
        char slave[A20_IFNAMSIZ];
        char newname[A20_IFNAMSIZ];
        void *data;
        short flags;
        int ivalue;
    } ifr_ifru;
};

/* User space indexes the SIOCGIFCONF buffer with its own libc definition of
 * this struct, so the size must match musl/glibc exactly.  It is 40 bytes on
 * LP64 and 32 on ILP32, so one hardcoded constant would be wrong on one of
 * them and both are asserted instead.  A flat `sizeof == 32` assertion is what
 * let the original mismatch ship: it passed while every real caller was
 * reading garbage. */
_Static_assert(sizeof(void *) == 8 ? sizeof(struct a20_ifreq) == 40
                                   : sizeof(struct a20_ifreq) == 32,
               "struct ifreq wire layout must match Linux/musl");
_Static_assert(offsetof(struct a20_ifreq, ifr_ifru) == 16, "ifreq union offset");

/* Linux `struct ifconf`: ifc_len is in/out.  With a NULL ifc_buf the caller is
 * asking how large a full listing would be, which is why the size query below
 * is computed exactly rather than estimated. */
struct a20_ifconf {
    int   ifc_len;
    void *ifc_buf;
};

/* Fill one SIOCGIFCONF row: interface name plus its IPv4 address.
 *
 * A netif's identity here is (name[0], name[1], num), and lwIP's netif_find
 * parses the number out of name[2] -- it rejects a name with no digit there.
 * The reported name is therefore composed rather than copied, which is also
 * the spelling /proc/net/status already uses and the only one
 * net_ifreq_lookup can resolve. */
static void net_ifreq_fill(struct a20_ifreq *ifr, const struct netif *nif)
{
    memset(ifr, 0, sizeof(*ifr));
    snprintf(ifr->ifr_name, sizeof(ifr->ifr_name), "%c%c%u",
             nif->name[0], nif->name[1], nif->num);

    /* The row carries a `struct sockaddr_in`.  sin_family is a raw host-order
     * constant -- Linux code writes `sin_family = AF_INET` and every libc
     * header documents it that way, so it must not be byteswapped.  sin_addr
     * is the part that was wrong: it lives at offset 4, after sin_family and
     * sin_port, and writing it at offset 2 lands in sin_port, which is how
     * 10.0.2.15 was reported as 2.15.0.0. */
    ifr->ifr_ifru.addr.sa_family = A20_AF_INET;
    ifr->ifr_ifru.addr.sa_port = 0;
    memcpy(&ifr->ifr_ifru.addr.sa_addr, &nif->ip_addr, sizeof(nif->ip_addr));
}

/* SIOCGIFCONF: enumerate the configured interfaces.  getifaddrs(), ifconfig
 * and busybox `ip` are all built on it, and the per-interface getters above
 * are unreachable without it. */
static int net_ifreq_getconf(void *uarg)
{
    struct a20_ifconf ifc;
    if (copy_from_user(&ifc, uarg, sizeof(ifc)) < 0)
        return -EFAULT;
    if (ifc.ifc_len < 0)
        return -EINVAL;

    int needed = 0;
    for (struct netif *n = netif_list; n; n = n->next)
        needed += (int)sizeof(struct a20_ifreq);

    if (!ifc.ifc_buf) {
        ifc.ifc_len = needed;
        return copy_to_user(uarg, &ifc, sizeof(ifc)) < 0 ? -EFAULT : 0;
    }

    /* Linux reads the wanted address family from the first entry of a
     * non-empty buffer.  Only IPv4 is reported, so a caller asking for
     * another family gets an empty list rather than IPv4 rows wearing the
     * wrong family tag. */
    if (ifc.ifc_len >= (int)sizeof(struct a20_ifreq)) {
        struct a20_ifreq probe;
        if (copy_from_user(&probe, ifc.ifc_buf, sizeof(probe)) < 0)
            return -EFAULT;
        uint16_t want = (uint16_t)probe.ifr_ifru.raw[0] |
                        ((uint16_t)probe.ifr_ifru.raw[1] << 8);
        if (want != A20_AF_UNSPEC && want != A20_AF_INET) {
            ifc.ifc_len = 0;
            return copy_to_user(uarg, &ifc, sizeof(ifc)) < 0 ? -EFAULT : 0;
        }
    }

    int limit = ifc.ifc_len;
    int used = 0;
    for (struct netif *n = netif_list; n; n = n->next) {
        if (used + (int)sizeof(struct a20_ifreq) > limit)
            break;
        struct a20_ifreq ifr;
        net_ifreq_fill(&ifr, n);
        if (copy_to_user((uint8_t *)ifc.ifc_buf + used, &ifr, sizeof(ifr)) < 0)
            return -EFAULT;
        used += (int)sizeof(ifr);
    }
    ifc.ifc_len = used;
    return copy_to_user(uarg, &ifc, sizeof(ifc)) < 0 ? -EFAULT : 0;
}

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
    /* sockaddr_in, not a two-byte-family header: sin_family, sin_port, then
     * sin_addr at offset 4.  Writing the address at offset 2 puts it in
     * sin_port and shifts every address by two bytes, so 10.0.2.15 was read
     * back as 2.15.0.0. */
    out[0] = (uint8_t)family;
    out[1] = (uint8_t)(family >> 8);
    if (n > sizeof(out) - 4)
        n = sizeof(out) - 4;
    memcpy(out + 4, bytes, n);
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
        /* Not netif_name_to_index(): netif->name is a two-character
         * abbreviation, not a NUL-terminated name, so that lookup compared
         * against garbage and every interface answered 0.  This is also the
         * numbering the netlink path and a20_lwip_netif_by_index() use. */
        int idx = (int)netif_get_index(nif);
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
    if (req == A20_SIOCGIFCONF)
        return net_ifreq_getconf(arg);
    switch (req) {
    case A20_SIOCGIFNAME: case A20_SIOCGIFFLAGS:
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

int net_socket_install_file(net_socket_t *s, int flags) {
    vfile_t *vf = vfile_alloc();
    if (!vf)
        return -ENOMEM;
    vf->flags = flags;
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_net_ops;
    vf->priv = s;

    int gfd = fdtable_install_current_vfile(vf, flags);
    if (gfd < 0) {
        net_socket_close_file(vf);
        vfile_free(vf);
        return gfd;
    }
    /* s->gfd now carries the socket's owning task fd; the socket's EventQ
     * identity is the vfile pointer itself. */
    s->gfd = gfd;
    s->vf = vf;
    return gfd;
}

/*
 * Push an event to every event queue watching this socket's global fd
 * (Native ABI event source wiring, docs/native-abi/09-… §5).  Cheap when no
 * Native process watches the socket.  Called from the net core; safe with a
 * bucket lock held (a20_event_notify takes its own IRQ-safe locks).
 */
void net_event_notify(net_socket_t *s, uint32_t event, uint64_t data0,
                      uint64_t data1)
{
    if (!s || !s->vf)
        return;
    a20_event_notify(s->vf, A20_OBJ_SOCKET, event,
                     data0, data1);
}
