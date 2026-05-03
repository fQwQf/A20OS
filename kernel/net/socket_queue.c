#include "net/socket_internal.h"
#include "mm/objcache.h"
#include "sys/bpf.h"
#include "core/string.h"

static obj_cache_t g_net_msg_cache = OBJ_CACHE_INIT("net_msg", net_msg_t, 16);

net_msg_t *net_msg_alloc(void)
{
    return (net_msg_t *)obj_cache_alloc_zero(&g_net_msg_cache);
}

void net_msg_free(net_msg_t *m)
{
    obj_cache_free(&g_net_msg_cache, m);
}

static void net_wake_socket_waiter_locked(net_socket_t *s)
{
    if (s->waiter && s->waiter->state == PROC_BLOCKED)
        proc_make_ready(s->waiter);
}

int net_enqueue_msg_locked(net_socket_t *dst, const void *buf, size_t len,
                           const void *addr, size_t addrlen)
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
        return -ENOMEM;
    memset(m, 0, sizeof(*m));
    memcpy(m->data, buf, len);
    m->len = len;
    if (addr && addrlen) {
        if (addrlen > NET_SOCKADDR_MAX)
            addrlen = NET_SOCKADDR_MAX;
        memcpy(m->addr, addr, addrlen);
        m->addrlen = addrlen;
    }
    if (dst->rx_tail)
        dst->rx_tail->next = m;
    else
        dst->rx_head = m;
    dst->rx_tail = m;
    dst->rx_count++;
    net_wake_socket_waiter_locked(dst);
    return (int)len;
}

int net_dequeue_msg_locked(net_socket_t *s, void *buf, size_t len,
                           void *addr, size_t *addrlen)
{
    net_msg_t *m = s->rx_head;
    if (!m) {
        if (s->closed)
            return 0;
        return -EAGAIN;
    }
    size_t n = m->len < len ? m->len : len;
    memcpy(buf, m->data, n);
    if (addr && addrlen && *addrlen > 0) {
        size_t alen = m->addrlen < *addrlen ? m->addrlen : *addrlen;
        memcpy(addr, m->addr, alen);
        *addrlen = alen;
    }

    if (s->type == SOCK_STREAM && n < m->len) {
        memmove(m->data, m->data + n, m->len - n);
        m->len -= n;
        return (int)n;
    }

    s->rx_head = m->next;
    if (!s->rx_head)
        s->rx_tail = NULL;
    s->rx_count--;
    net_msg_free(m);
    return (int)n;
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
    net_wake_socket_waiter_locked(listener);
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
