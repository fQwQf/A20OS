#include "net/socket_internal.h"
#include "net/socket_side.h"
#include "fs/file.h"
#include "mm/objcache.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "core/stdio.h"
#include "lwip/pbuf.h"

static obj_cache_t g_net_msg_cache = OBJ_CACHE_INIT("net_msg", net_msg_t, 16);

/*
 * Running receive-queue byte count.
 *
 * This used to be a NET_MAX_SOCKETS-entry array indexed by registry slot --
 * 512 KiB of table on the server profile, holding one number per socket.  It
 * lives in net_socket_t now (rxq_tally) and is written only under that
 * socket's lock, which is what makes the two halves below impossible to
 * observe from two CPUs at once.  The low 32 bits are the payload bytes still
 * readable, the high 32 the message count.
 *
 * The queue is capped at NET_MAX_QUEUE messages of at most NET_MAX_PAYLOAD
 * bytes, so 32 bits of byte count has two orders of magnitude of headroom and
 * the pair fits a single load.  Answering FIONREAD by summing the queue
 * instead costs one message per entry, and NET_MAX_QUEUE is 128 on the
 * default profile and 1024 on the server one.
 *
 * Carrying the message count next to the byte count is what makes the tally
 * safe rather than merely fast.  Everything that appends to or removes from the
 * queue moves both halves together, so they agree; a teardown that empties the
 * queue without going through net_msg_link_locked() -- shutdown(SHUT_RD)
 * clears rx_head and rx_count in one step -- leaves them disagreeing, and every
 * entry point below checks the pair instead of trusting the byte half, so such
 * a teardown costs one rebuild rather than a permanently wrong total.
 */

static size_t net_rxq_sum_locked(const net_socket_t *s)
{
    size_t total = 0;
    for (const net_msg_t *m = s->rx_head; m; m = m->next)
        total += (s->type == SOCK_STREAM) ? (m->len - m->off) : m->len;
    return total;
}

void net_rxq_reset_locked(net_socket_t *s)
{
    if (s)
        s->rxq_tally = 0;
}

void net_rxq_bytes_added_locked(net_socket_t *s, size_t bytes)
{
    uint64_t t = s->rxq_tally;
    /* The queue grew by one message, so the tally must still describe the
     * pre-growth queue. */
    if ((int)(t >> 32) != s->rx_count - 1)
        t = (uint64_t)(uint32_t)net_rxq_sum_locked(s);
    s->rxq_tally = (t & 0xffffffff00000000ULL) |
                   ((uint32_t)t + (uint32_t)bytes);
}

/* Called before the caller drops rx_count, so the tally still describes the
 * queue the message is being taken from.  A partial read leaves the message in
 * place and only the byte half moves. */
void net_rxq_bytes_removed_locked(net_socket_t *s, size_t bytes)
{
    uint64_t t = s->rxq_tally;
    if ((int)(t >> 32) != s->rx_count)
        t = (uint64_t)(uint32_t)net_rxq_sum_locked(s);
    uint32_t have = (uint32_t)t;
    s->rxq_tally = (t & 0xffffffff00000000ULL) |
                   (have - (uint32_t)bytes < have ? have - (uint32_t)bytes
                                                 : 0);
}

size_t net_rxq_bytes_locked(net_socket_t *s)
{
    uint64_t t = s->rxq_tally;
    if ((int)(t >> 32) != s->rx_count) {
        t = (uint64_t)(uint32_t)net_rxq_sum_locked(s);
        s->rxq_tally = ((uint64_t)(uint32_t)s->rx_count << 32) | (uint32_t)t;
    }
    return (size_t)(uint32_t)t;
}

/*
 * Capture the current task credentials for SCM_CREDENTIALS.  Called under the
 * destination socket's lock while that socket is being serviced;
 * proc_current() is safe there (it is a CPU-local read).
 */
static void net_capture_sender_cred(net_msg_t *m)
{
    task_t *cur = proc_current();
    if (!cur)
        return;
    m->has_cred = 1;
    m->cred_pid = cur->pid;
    m->cred_uid = cur->cred.uid;
    m->cred_gid = cur->cred.gid;
}

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
    kfree(m->overflow);
    obj_cache_free(&g_net_msg_cache, m);
}

void net_scm_drop_files(vfile_t **files, int nfiles)
{
    if (!files)
        return;
    for (int i = 0; i < nfiles; i++)
        vfs_put_file(files[i]);
}

/* Returns the message's payload base, or NULL if the length is out of range or
 * the overflow allocation fails.  obj_cache_alloc_zero already zeroes the
 * object, so the inline buffer needs no further clearing. */
static uint8_t *net_msg_alloc_payload(net_msg_t **out, size_t len)
{
    if (len > NET_MAX_PAYLOAD)
        return NULL;
    net_msg_t *m = net_msg_alloc();
    if (!m)
        return NULL;
    if (len > NET_MSG_INLINE_PAYLOAD) {
        m->overflow = kmalloc(len);
        if (!m->overflow) {
            net_msg_free(m);
            return NULL;
        }
    }
    m->len = len;
    m->off = 0;
    *out = m;
    return net_msg_payload(m);
}

/* Shared tail of every enqueue: address, ancillary metadata, sender
 * credentials, then the append onto the receive queue. */
static void net_msg_link_locked(net_socket_t *dst, net_msg_t *m,
                                const void *addr, size_t addrlen,
                                const net_bh_event_t *meta)
{
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
    net_capture_sender_cred(m);
    if (dst->rx_tail)
        dst->rx_tail->next = m;
    else
        dst->rx_head = m;
    dst->rx_tail = m;
    dst->rx_count++;
    net_rxq_bytes_added_locked(dst, m->len);
}

int net_enqueue_msg_locked_meta(net_socket_t *dst, const void *buf, size_t len,
                                const void *addr, size_t addrlen,
                                const net_bh_event_t *meta)
{
    if (!dst || dst->closed)
        return net_notconn(NET_NOTCONN_ENQUEUE_META);
    if (len > NET_MAX_PAYLOAD)
        return -EMSGSIZE;
    if (dst->rx_count >= NET_MAX_QUEUE)
        return -EAGAIN;
    net_msg_t *m = NULL;
    uint8_t *payload = net_msg_alloc_payload(&m, len);
    if (!payload)
        return -EAGAIN;
    memcpy(payload, buf, len);
    net_msg_link_locked(dst, m, addr, addrlen, meta);
    return (int)len;
}

/* Enqueue straight out of a pbuf, for a bottom-half event too large to stage
 * inline.  The pbuf stays owned by the ring (see net_bh_ring_t.owned); this
 * only borrows it for the duration of the copy. */
int net_enqueue_msg_locked_pbuf(net_socket_t *dst, const struct pbuf *p,
                                uint32_t off, size_t len,
                                const void *addr, size_t addrlen,
                                const net_bh_event_t *meta)
{
    if (!dst || dst->closed)
        return net_notconn(NET_NOTCONN_ENQUEUE_PBUF);
    if (!p || len > NET_MAX_PAYLOAD)
        return -EMSGSIZE;
    if (off > (uint32_t)p->tot_len || len > (size_t)p->tot_len - off)
        return -EMSGSIZE;
    if (dst->rx_count >= NET_MAX_QUEUE)
        return -EAGAIN;
    net_msg_t *m = NULL;
    uint8_t *payload = net_msg_alloc_payload(&m, len);
    if (!payload)
        return -EAGAIN;
    if (pbuf_copy_partial(p, payload, (u16_t)len, (u16_t)off) != ERR_OK) {
        net_msg_free(m);
        return -EAGAIN;
    }
    net_msg_link_locked(dst, m, addr, addrlen, meta);
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
        /* Two sockets, ascending by address.  Both are pinned by their caller's
         * references, so neither can be freed under us here. */
        net_sock_pair_t pair = net_sock_lock2(s, dst);
        if (!net_socket_is_live(s) || !net_socket_is_live(dst)) {
            net_sock_unlock2(pair);
            return net_notconn(NET_NOTCONN_BLOCKING_PRE_PARK_RACE);
        }
        /* UDP connect sets peer_addr but NOT s->peer, so s->peer is
           legitimately NULL — skip this check for DGRAM. */
        if (s->connected && s->peer != dst &&
            s->type != SOCK_DGRAM) {
            net_sock_unlock2(pair);
            return net_notconn(NET_NOTCONN_BLOCKING_PEER_MISMATCH);
        }
        int r = net_enqueue_msg_locked(dst, buf, len, addr, addrlen);
        if (r != -EAGAIN || dontwait) {
            if (r >= 0) {
                net_event_notify(dst, A20_EVENT_READABLE, 0, 0);
                (void)wait_queue_collect_one(
                    &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            }
            net_sock_unlock2(pair);
            (void)proc_wake_q_flush(&wake_q);
            return r;
        }
        task_t *cur = proc_current();
        if (!cur) {
            net_sock_unlock2(pair);
            return -EAGAIN;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_sock_unlock2(pair);
            return -ERESTARTSYS;
        }
        if (timeout_ticks &&
            (int64_t)(timer_get_ticks() - (start + timeout_ticks)) >= 0) {
            net_sock_unlock2(pair);
            return -EAGAIN;
        }
        if (!timeout_ticks && s->type == SOCK_DGRAM) {
            uint64_t udp_deadline = start + MS_TO_TICKS(200);
            if ((int64_t)(timer_get_ticks() - udp_deadline) >= 0) {
                net_sock_unlock2(pair);
                return -EAGAIN;
            }
        }
        if (!timeout_ticks && s->type == SOCK_STREAM) {
            uint64_t tcp_deadline = start + MS_TO_TICKS(5000);
            if ((int64_t)(timer_get_ticks() - tcp_deadline) >= 0) {
                net_sock_unlock2(pair);
                return -EAGAIN;
            }
        }
        uint64_t deadline = timeout_ticks ? start + timeout_ticks : 0;
        if (!deadline && s->type == SOCK_DGRAM)
            deadline = start + MS_TO_TICKS(200);
        if (!deadline && s->type == SOCK_STREAM)
            deadline = start + MS_TO_TICKS(5000);
        net_sock_unlock2(pair);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        pair = net_sock_lock2(s, dst);
        if (!net_socket_is_live(s) || !net_socket_is_live(dst) ||
            (s->connected && s->peer != dst && s->type != SOCK_DGRAM)) {
            net_sock_unlock2(pair);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return net_notconn(NET_NOTCONN_BLOCKING_POST_PARK_RACE);
        }
        r = net_enqueue_msg_locked(dst, buf, len, addr, addrlen);
        if (r != -EAGAIN) {
            if (r >= 0) {
                net_event_notify(dst, A20_EVENT_READABLE, 0, 0);
                (void)wait_queue_collect_one(
                    &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            }
            net_sock_unlock2(pair);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            (void)proc_wake_q_flush(&wake_q);
            return r;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_sock_unlock2(pair);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ERESTARTSYS;
        }
        bool linked =
            wait_queue_link(&dst->write_waitq, &entry, token, 0);
        net_sock_unlock2(pair);
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
    memcpy(buf, net_msg_payload(m) + m->off, n);
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
        meta->has_cred = m->has_cred;
        meta->cred_pid = m->cred_pid;
        meta->cred_uid = m->cred_uid;
        meta->cred_gid = m->cred_gid;
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
        net_rxq_bytes_removed_locked(s, n);
        return (int)n;
    }

    net_rxq_bytes_removed_locked(s, avail);
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
