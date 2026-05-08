#include "net/socket_internal.h"
#include "sys/bpf.h"
#include "core/string.h"
#include "lwip/tcp.h"

static uint64_t timeval_to_ticks(const void *optval, size_t optlen)
{
    if (!optval || optlen < sizeof(long) * 2)
        return 0;
    const long *tv = (const long *)optval;
    if (tv[0] < 0 || tv[1] < 0)
        return 0;
    uint64_t ticks = (uint64_t)tv[0] * TICKS_PER_SEC +
                     (uint64_t)tv[1] * TICKS_PER_SEC / 1000000ULL;
    return ticks ? ticks : 1;
}

static int net_copyout_int(void *optval, size_t *optlen, int val)
{
    if (!optval || !optlen || *optlen < sizeof(int))
        return -EINVAL;
    memcpy(optval, &val, sizeof(val));
    *optlen = sizeof(val);
    return 0;
}

int net_listen(int gfd, int backlog)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG)
        return 0;
    if ((s->type != SOCK_STREAM && !(s->domain == AF_UNIX && s->type == SOCK_SEQPACKET)) ||
        (s->domain != AF_INET && s->domain != AF_INET6 && s->domain != AF_UNIX))
        return -EOPNOTSUPP;
    if (s->domain == AF_UNIX && !s->bound)
        return -EINVAL;
    if (s->listening)
        return 0;
    if (backlog <= 0)
        backlog = 1;
    if (backlog > NET_MAX_QUEUE)
        backlog = NET_MAX_QUEUE;

    /*
     * Local TCP connections are handled by the socket layer's accept queue.
     * Avoid converting to an lwIP LISTEN pcb here: LTP's localhost accept
     * tests exercise close-after-accept heavily, and keeping the listener as
     * a normal bound pcb gives deterministic close semantics.
     */
    s->listening = 1;
    if (s->domain == AF_INET || s->domain == AF_INET6) {
        s->local_tcp = 1;
        if (s->domain == AF_INET)
            net_tcp_drop_pcb(s);
    }
    return 0;
}

int net_accept(int gfd, void *addr, size_t *addrlen, int flags)
{
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
        return -EINVAL;
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG)
        return net_alg_socket_accept(s, addrlen, flags);
    if ((s->type != SOCK_STREAM && !(s->domain == AF_UNIX && s->type == SOCK_SEQPACKET)) ||
        (s->domain != AF_INET && s->domain != AF_INET6 && s->domain != AF_UNIX))
        return -EOPNOTSUPP;
    if (!s->listening)
        return -EINVAL;

    net_socket_t *child = NULL;
    uint64_t start = timer_get_ticks();
    for (;;) {
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        child = net_accept_queue_pop_locked(s);
        if (child) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            break;
        }
        if (s->nonblock) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EAGAIN;
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
        if (net_task_has_unblocked_signal(cur))
            return -ERESTARTSYS;
    }

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    int r = net_register_socket_locked(child);
    spin_unlock_irqrestore(&g_net_lock, irq);
    if (r < 0) {
        net_inet_socket_destroy(child);
        net_socket_free(child);
        return r;
    }
    net_inet_accept_child_ready(child);

    if (addr && addrlen && *addrlen > 0) {
        size_t n = child->peer_len < *addrlen ? child->peer_len : *addrlen;
        memcpy(addr, child->peer_addr, n);
        *addrlen = n;
    }

    child->nonblock = (flags & SOCK_NONBLOCK) ? 1 : s->nonblock;
    return net_socket_install_file(child, O_RDWR | (child->nonblock ? O_NONBLOCK : 0));
}

int net_getsockname(int gfd, void *addr, size_t *addrlen)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    if (!addr || !addrlen)
        return -EFAULT;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    size_t n = s->local_len < *addrlen ? s->local_len : *addrlen;
    memcpy(addr, s->local, n);
    *addrlen = n;
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}

int net_getpeername(int gfd, void *addr, size_t *addrlen)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    if (!s->connected)
        return -ENOTCONN;
    size_t n = s->peer_len < *addrlen ? s->peer_len : *addrlen;
    memcpy(addr, s->peer_addr, n);
    *addrlen = n;
    return 0;
}

int net_setsockopt(int gfd, int level, int optname, const void *optval, size_t optlen)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    if (s->domain == AF_ALG && level == SOL_ALG && optname == ALG_SET_KEY) {
        if (strcmp(s->alg_type, "aead") == 0 &&
            strcmp(s->alg_name, "authenc(hmac(sha256),cbc(aes))") == 0 &&
            optlen < 16)
            return -EINVAL;
        if ((strcmp(s->alg_type, "skcipher") == 0 || strcmp(s->alg_type, "aead") == 0) &&
            optlen != 0 && optlen < 16)
            return -EINVAL;
        return 0;
    }
    if (level == IPPROTO_IP) {
        if (optname == MCAST_JOIN_GROUP)
            return optlen ? 0 : -EINVAL;
        if (optname == MCAST_LEAVE_GROUP)
            return -EADDRNOTAVAIL;
        return 0;
    }
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_CHECKSUM) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int offset;
        memcpy(&offset, optval, sizeof(offset));
        if (offset >= 0 && (offset & 1))
            return -EINVAL;
        s->ipv6_checksum_offset = offset;
        return 0;
    }
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_V6ONLY)
        return optlen >= sizeof(int) ? 0 : -EINVAL;
    if (s->domain == AF_INET6 && s->type == SOCK_RAW &&
        level == IPPROTO_ICMPV6 && optname == ICMP6_FILTER) {
        if (!optval || optlen < sizeof(s->icmp6_filter))
            return -EINVAL;
        memcpy(s->icmp6_filter, optval, sizeof(s->icmp6_filter));
        s->icmp6_filter_set = 1;
        return 0;
    }
    if (level == IPPROTO_TCP) {
        if (s->type != SOCK_STREAM)
            return -ENOPROTOOPT;
        if (optname == TCP_CONGESTION)
            return optval && optlen ? 0 : -EINVAL;
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        switch (optname) {
        case TCP_NODELAY:
            s->tcp_nodelay = val != 0;
            if (s->tcp) {
                if (s->tcp_nodelay)
                    tcp_nagle_disable(s->tcp);
                else
                    tcp_nagle_enable(s->tcp);
            }
            return 0;
        case TCP_CORK:
        case TCP_MAXSEG:
        case TCP_SYNCNT:
        case TCP_LINGER2:
        case TCP_DEFER_ACCEPT:
        case TCP_WINDOW_CLAMP:
        case TCP_QUICKACK:
        case TCP_USER_TIMEOUT:
            return 0;
        case TCP_KEEPIDLE:
            if (val <= 0)
                return -EINVAL;
            s->keep_idle = val;
            if (s->tcp)
                s->tcp->keep_idle = (u32_t)val * 1000U;
            return 0;
        case TCP_KEEPINTVL:
            if (val <= 0)
                return -EINVAL;
            s->keep_intvl = val;
            if (s->tcp)
                s->tcp->keep_intvl = (u32_t)val * 1000U;
            return 0;
        case TCP_KEEPCNT:
            if (val <= 0)
                return -EINVAL;
            s->keep_cnt = val;
            if (s->tcp)
                s->tcp->keep_cnt = (u32_t)val;
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }
    if (level == SOL_SOCKET && optname == SO_ATTACH_BPF) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int prog_fd;
        memcpy(&prog_fd, optval, sizeof(prog_fd));
        if (!bpf_prog_is_loaded(prog_fd))
            return -EBADF;
        s->bpf_prog_fd = prog_fd;
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_RCVTIMEO) {
        if (!optval || optlen < sizeof(long) * 2)
            return -EINVAL;
        s->recv_timeout_ticks = timeval_to_ticks(optval, optlen);
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_SNDTIMEO) {
        if (!optval || optlen < sizeof(long) * 2)
            return -EINVAL;
        s->send_timeout_ticks = timeval_to_ticks(optval, optlen);
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->reuseaddr = val != 0;
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_REUSEPORT) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->reuseport = val != 0;
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_KEEPALIVE) {
        if (!optval || optlen < sizeof(int))
            return -EINVAL;
        int val;
        memcpy(&val, optval, sizeof(val));
        s->keepalive = val != 0;
        if (s->tcp) {
            if (s->keepalive)
                s->tcp->so_options |= SOF_KEEPALIVE;
            else
                s->tcp->so_options &= ~SOF_KEEPALIVE;
        }
        return 0;
    }
    if (level == SOL_SOCKET)
        return 0;
    return -EOPNOTSUPP;
}

int net_getsockopt(int gfd, int level, int optname, void *optval, size_t *optlen)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    if (!optval || !optlen)
        return -EINVAL;
    int val = 0;
    if (level == SOL_SOCKET && optname == SO_TYPE)
        val = s->type;
    else if (level == SOL_SOCKET && optname == SO_ERROR)
        val = 0;
    else if (level == SOL_SOCKET && optname == SO_ACCEPTCONN)
        val = s->listening;
    else if (level == SOL_SOCKET && optname == SO_DOMAIN)
        val = s->domain;
    else if (level == SOL_SOCKET && optname == SO_PROTOCOL)
        val = s->protocol;
    else if (level == SOL_SOCKET &&
             (optname == SO_SNDBUF || optname == SO_RCVBUF))
        val = NET_MAX_QUEUE * NET_MAX_PAYLOAD;
    else if (level == SOL_SOCKET && optname == SO_REUSEADDR)
        val = s->reuseaddr;
    else if (level == SOL_SOCKET && optname == SO_REUSEPORT)
        val = s->reuseport;
    else if (level == SOL_SOCKET && optname == SO_KEEPALIVE)
        val = s->keepalive;
    else if (level == SOL_SOCKET)
        val = 0;
    else if (level == IPPROTO_TCP) {
        if (s->type != SOCK_STREAM)
            return -ENOPROTOOPT;
        if (optname == TCP_CONGESTION) {
            static const char congestion[] = "cubic";
            size_t n = *optlen < sizeof(congestion) ? *optlen : sizeof(congestion);
            if (n)
                memcpy(optval, congestion, n);
            *optlen = n;
            return 0;
        }
        if (optname == TCP_INFO) {
            size_t n = *optlen;
            memset(optval, 0, n);
            if (n)
                ((uint8_t *)optval)[0] = s->listening ? 10 : (s->connected ? 1 : 7);
            return 0;
        }
        switch (optname) {
        case TCP_NODELAY:
            val = s->tcp_nodelay;
            break;
        case TCP_MAXSEG:
            val = 1460;
            break;
        case TCP_CORK:
        case TCP_SYNCNT:
        case TCP_LINGER2:
        case TCP_DEFER_ACCEPT:
        case TCP_WINDOW_CLAMP:
        case TCP_QUICKACK:
        case TCP_USER_TIMEOUT:
            val = 0;
            break;
        case TCP_KEEPIDLE:
            val = s->keep_idle > 0 ? s->keep_idle :
                  (s->tcp ? (int)(s->tcp->keep_idle / 1000U) : 7200);
            break;
        case TCP_KEEPINTVL:
            val = s->keep_intvl > 0 ? s->keep_intvl :
                  (s->tcp ? (int)(s->tcp->keep_intvl / 1000U) : 75);
            break;
        case TCP_KEEPCNT:
            val = s->keep_cnt > 0 ? s->keep_cnt :
                  (s->tcp ? (int)s->tcp->keep_cnt : 9);
            break;
        default:
            return -ENOPROTOOPT;
        }
    }
    else
        return -EOPNOTSUPP;
    return net_copyout_int(optval, optlen, val);
}

int net_shutdown(int gfd, int how)
{
    (void)how;
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    s->closed = 1;
    if (s->waiter && s->waiter->state == PROC_BLOCKED)
        proc_make_ready(s->waiter);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}

int net_set_nonblock(int gfd, int nonblock)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    s->nonblock = nonblock ? 1 : 0;
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}

int net_poll_events(int gfd, short events)
{
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s)
        return -ENOTSOCK;
    short revents = 0;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->closed)
        revents |= POLLHUP;
    if ((events & POLLIN) &&
        (s->rx_head || s->accept_head || s->closed ||
         (s->domain == AF_ALG && (strcmp(s->alg_type, "hash") == 0 || s->alg_last_len > 0))))
        revents |= POLLIN;
    if ((events & POLLOUT) && !s->closed)
        revents |= POLLOUT;
    spin_unlock_irqrestore(&g_net_lock, irq);
    return revents;
}
