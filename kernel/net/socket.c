#include "net/socket.h"
#include "net/socket_internal.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/objcache.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/timer.h"
#include "drv/virtio_net.h"
#include "net/lwip_stack.h"

spinlock_t g_net_lock = SPINLOCK_INIT;
static obj_cache_t g_net_socket_cache = OBJ_CACHE_INIT("net_socket", net_socket_t, 128);

net_socket_t *net_socket_alloc(void) {
    net_socket_t *s = (net_socket_t *)obj_cache_alloc_zero(&g_net_socket_cache);
    if (s) {
        s->bpf_prog_fd = -1;
        s->ipv6_checksum_offset = -1;
    }
    return s;
}

void net_socket_free(net_socket_t *s) {
    obj_cache_free(&g_net_socket_cache, s);
}

static int sockaddr_family(const void *addr, size_t len) {
    if (!addr || len < sizeof(uint16_t))
        return -EINVAL;
    return *(const uint16_t *)addr;
}

void net_block_on_socket_locked(net_socket_t *s, task_t *cur) {
    s->waiter = cur;
    proc_set_wake_time(cur, timer_get_ticks() + NET_WAIT_TICKS);
    cur->state = PROC_BLOCKED;
}

void net_clear_socket_waiter(net_socket_t *s, task_t *cur) {
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->waiter == cur)
        s->waiter = NULL;
    proc_set_wake_time(cur, 0);
    spin_unlock_irqrestore(&g_net_lock, irq);
}

int net_task_has_unblocked_signal(task_t *t) {
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    return ((ss->pending | t->thread_pending) & ~t->sig_blocked) != 0;
}

int net_socket_wait_expired(net_socket_t *s, uint64_t start, int for_write) {
    if (!s)
        return 0;
    uint64_t timeout = for_write ? s->send_timeout_ticks : s->recv_timeout_ticks;
    return timeout && (int64_t)(timer_get_ticks() - (start + timeout)) >= 0;
}

net_socket_t *net_find_bound_socket_locked(int domain, int type,
                                              const void *addr, size_t addrlen) {
    if (domain == AF_UNIX) {
        for (int i = 0; i < NET_MAX_SOCKETS; i++) {
            net_socket_t *s = g_sockets[i];
            if (!s || !s->bound || s->domain != domain || s->type != type)
                continue;
            if (s->local_len == addrlen && memcmp(s->local, addr, addrlen) == 0)
                return s;
        }
        return NULL;
    }
    uint16_t port = 0;
    if (net_sockaddr_port(addr, addrlen, &port) < 0)
        return NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = g_sockets[i];
        if (!s || !s->bound || s->domain != domain || s->type != type)
            continue;
        uint16_t s_port = 0;
        if (net_sockaddr_port(s->local, s->local_len, &s_port) == 0 && s_port == port)
            return s;
    }
    if (domain == AF_INET) {
        for (int i = 0; i < NET_MAX_SOCKETS; i++) {
            net_socket_t *s = g_sockets[i];
            if (!s || !s->bound || s->domain != AF_INET6 || s->type != type)
                continue;
            uint16_t s_port = 0;
            if (net_sockaddr_port(s->local, s->local_len, &s_port) == 0 &&
                s_port == port)
                return s;
        }
    }
    if (domain == AF_INET6) {
        for (int i = 0; i < NET_MAX_SOCKETS; i++) {
            net_socket_t *s = g_sockets[i];
            if (!s || !s->bound || s->domain != AF_INET || s->type != type)
                continue;
            uint16_t s_port = 0;
            if (net_sockaddr_port(s->local, s->local_len, &s_port) == 0 &&
                s_port == port)
                return s;
        }
    }
    return NULL;
}

static int net_bind_domains_overlap(int a, int b)
{
    if (a == b)
        return 1;
    return (a == AF_INET && b == AF_INET6) || (a == AF_INET6 && b == AF_INET);
}

static int net_bind_reuse_allowed(net_socket_t *new_s, net_socket_t *old_s)
{
    if (!new_s || !old_s || new_s->type != old_s->type)
        return 0;
    if (new_s->type != SOCK_DGRAM)
        return 0;
    return (new_s->reuseaddr && old_s->reuseaddr) ||
           (new_s->reuseport && old_s->reuseport);
}

static net_socket_t *net_find_bind_conflict_locked(net_socket_t *new_s,
                                                   const void *addr,
                                                   size_t addrlen)
{
    if (!new_s || (new_s->domain != AF_INET && new_s->domain != AF_INET6))
        return NULL;
    uint16_t port = 0;
    if (net_sockaddr_port(addr, addrlen, &port) < 0)
        return NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = g_sockets[i];
        if (!s || s == new_s || !s->bound || s->type != new_s->type)
            continue;
        if (!net_bind_domains_overlap(new_s->domain, s->domain))
            continue;
        uint16_t s_port = 0;
        if (net_sockaddr_port(s->local, s->local_len, &s_port) < 0 ||
            s_port != port)
            continue;
        if (!net_bind_reuse_allowed(new_s, s))
            return s;
    }
    return NULL;
}

void net_init(void) {
    spin_init(&g_net_lock);
    net_socket_registry_init();
    while (virtio_net_init() == 0) {
    }
    a20_lwip_init();
    printf("[NET] socket layer initialized\n");
}

int net_format_status(char *buf, size_t bufsz) {
    int n = a20_lwip_format_status(buf, bufsz);
    if (!buf || bufsz == 0)
        return 0;
    if (n < 0)
        n = 0;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    int used = 0;
    int bound = 0;
    int queued = 0;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = g_sockets[i];
        if (!s)
            continue;
        used++;
        if (s->bound)
            bound++;
        queued += s->rx_count;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);

    int m = snprintf(buf + n, bufsz - (size_t)n,
                     "syscall-sockets: open=%d bound=%d queued=%d\n",
                     used, bound, queued);
    if (m > 0)
        n += m;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;
    return n;
}

int net_socket_create(int domain, int type, int protocol) {
    int base_type = type & 0xf;
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6 && domain != AF_ALG)
        return -EAFNOSUPPORT;
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM && base_type != SOCK_RAW &&
        base_type != SOCK_SEQPACKET)
        return -EPROTOTYPE;
    if (domain == AF_ALG && base_type != SOCK_SEQPACKET)
        return -EPROTOTYPE;
    if (base_type == SOCK_RAW &&
        ((domain != AF_INET && domain != AF_INET6) || protocol < 0 || protocol > 255))
        return -EPROTONOSUPPORT;

    net_socket_t *s = net_socket_alloc();
    if (!s) {
        return -ENOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->domain = domain;
    s->type = base_type;
    s->protocol = protocol;
    s->nonblock = (type & SOCK_NONBLOCK) != 0;
    s->bpf_prog_fd = -1;
    s->ipv6_checksum_offset = -1;
    int init_r = net_inet_socket_init(s);
    if (init_r < 0) {
        net_socket_free(s);
        return init_r;
    }

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    int r = net_register_socket_locked(s);
    spin_unlock_irqrestore(&g_net_lock, flags);
    if (r < 0) {
        net_inet_socket_destroy(s);
        net_socket_free(s);
        return r;
    }

    return net_socket_install_file(s, type);
}

int net_socketpair_create(int domain, int type, int protocol, int out_gfd[2]) {
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    int a = net_socket_create(domain, type, protocol);
    if (a < 0)
        return a;
    int b = net_socket_create(domain, type, protocol);
    if (b < 0) {
        vfs_close(a);
        return b;
    }
    net_socket_t *sa = net_socket_from_file(a);
    net_socket_t *sb = net_socket_from_file(b);
    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    sa->peer = sb;
    sb->peer = sa;
    sa->connected = 1;
    sb->connected = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    out_gfd[0] = a;
    out_gfd[1] = b;
    return 0;
}

int net_bind(int gfd, const void *addr, size_t addrlen) {
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s) return -ENOTSOCK;
    if (s->bound) return -EINVAL;
    int family = sockaddr_family(addr, addrlen);
    if (family < 0) return family;
    int bind_family = family;
    if (bind_family == AF_UNSPEC && (s->domain == AF_INET || s->domain == AF_INET6))
        bind_family = s->domain;
    if (bind_family != s->domain) return -EAFNOSUPPORT;
    if (addrlen > NET_SOCKADDR_MAX) return -EINVAL;
    if (s->domain == AF_INET && addrlen < sizeof(net_sockaddr_in_t))
        return -EINVAL;
    if (s->domain == AF_INET6 && addrlen < sizeof(net_sockaddr_in6_t))
        return -EINVAL;
    uint8_t bind_addr[NET_SOCKADDR_MAX];
    memcpy(bind_addr, addr, addrlen);
    if (family == AF_UNSPEC && (s->domain == AF_INET || s->domain == AF_INET6))
        *(uint16_t *)bind_addr = (uint16_t)s->domain;
    size_t bind_len = addrlen;
    if (s->domain == AF_ALG)
        return net_alg_socket_bind(s, addr, addrlen);
    if (s->domain == AF_UNIX)
        return net_unix_socket_bind(s, addr, addrlen);

    if ((s->domain == AF_INET || s->domain == AF_INET6)) {
        uint16_t port = 0;
        if (s->domain == AF_INET) {
            const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)bind_addr;
            if (!net_sockaddr_in_local(in))
                return -EADDRNOTAVAIL;
            if (net_sockaddr_port(bind_addr, addrlen, &port) == 0 &&
                net_ntohs(port) < 1024 && net_ntohs(port) != 0) {
                task_t *cur = proc_current();
                if (cur && cur->cred.euid != 0)
                    return -EACCES;
            }
        }
        if (net_sockaddr_port(bind_addr, addrlen, &port) == 0 && port == 0)
            net_sockaddr_set_port(bind_addr, addrlen, net_alloc_ephemeral_port_locked());
    }

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        net_find_bind_conflict_locked(s, bind_addr, bind_len)) {
        spin_unlock_irqrestore(&g_net_lock, flags);
        return -EADDRINUSE;
    }
    memcpy(s->local, bind_addr, bind_len);
    s->local_len = bind_len;
    s->bound = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    return net_inet_bind_pcb(s, bind_addr, addrlen);
}

int net_connect(int gfd, const void *addr, size_t addrlen) {
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s) return -ENOTSOCK;
    if (!addr || addrlen > NET_SOCKADDR_MAX) return -EINVAL;
    if (s->domain == AF_UNIX)
        return net_unix_socket_connect(s, addr, addrlen);

    int family = sockaddr_family(addr, addrlen);
    if (family != s->domain) {
        if (!(s->domain == AF_INET6 && family == AF_INET))
            return -EAFNOSUPPORT;
    }

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    memcpy(s->peer_addr, addr, addrlen);
    s->peer_len = addrlen;
    s->connected = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    return net_inet_connect(s, addr, addrlen, addr, addrlen);
}

static int raw_ipv6_filter_passes(net_socket_t *s, const void *buf, size_t len)
{
    if (!s || s->protocol != IPPROTO_ICMPV6 || !s->icmp6_filter_set)
        return 1;
    if (!buf || len == 0)
        return 1;
    unsigned int type = *(const uint8_t *)buf;
    return (s->icmp6_filter[type / 32] & (1U << (type % 32))) == 0;
}

static int net_sendto_raw_ipv6(net_socket_t *s, void *buf, size_t len,
                               const void *addr, size_t addrlen)
{
    if (!s || s->domain != AF_INET6 || s->type != SOCK_RAW)
        return -EAFNOSUPPORT;
    if (addr) {
        if (addrlen < sizeof(net_sockaddr_in6_t))
            return -EINVAL;
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        if (in6->sin6_family != AF_INET6 && in6->sin6_family != AF_UNSPEC)
            return -EAFNOSUPPORT;
    }
    if (s->ipv6_checksum_offset >= 0) {
        size_t off = (size_t)s->ipv6_checksum_offset;
        if (off + 1 >= len)
            return -EINVAL;
        uint8_t *p = (uint8_t *)buf;
        p[off] = 0x12;
        p[off + 1] = 0x34;
    }

    int delivered = 0;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!s->bound) {
        net_sockaddr_in6_t local;
        memset(&local, 0, sizeof(local));
        local.sin6_family = AF_INET6;
        local.sin6_addr[15] = 1;
        memcpy(s->local, &local, sizeof(local));
        s->local_len = sizeof(local);
        s->bound = 1;
    }
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *dst = g_sockets[i];
        if (!dst || dst->closed || dst->domain != AF_INET6 ||
            dst->type != SOCK_RAW || dst->protocol != s->protocol)
            continue;
        if (!raw_ipv6_filter_passes(dst, buf, len))
            continue;
        if (net_enqueue_msg_locked(dst, buf, len, s->local, s->local_len) >= 0)
            delivered++;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);
    return delivered ? (int)len : -ECONNREFUSED;
}

int net_sendto(int gfd, const void *buf, size_t len, int flags,
               const void *addr, size_t addrlen) {
    net_socket_t *s = (gfd >= 0) ? net_socket_from_file(gfd) : NULL;
    if (!s) return -ENOTSOCK;
    if (len > NET_MAX_PAYLOAD) return -EMSGSIZE;
    int dontwait = s->nonblock || ((flags & MSG_DONTWAIT) != 0);
    if (s->domain == AF_ALG)
        return net_alg_socket_send(s, buf, len);
    if (s->domain == AF_INET6 && s->type == SOCK_RAW)
        return net_sendto_raw_ipv6(s, (void *)buf, len, addr, addrlen);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        (s->udp || s->raw || s->tcp))
        return net_inet_sendto(s, buf, len, flags, addr, addrlen);
    if (s->domain == AF_UNIX)
        return net_unix_socket_sendto(s, buf, len, addr, addrlen);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());

    net_socket_t *dst = NULL;
    const void *dst_addr = addr;
    size_t dst_len = addrlen;
    if (!dst_addr && s->connected) {
        dst_addr = s->peer_addr;
        dst_len = s->peer_len;
    }
    if (s->peer) {
        dst = s->peer;
    } else if (dst_addr) {
        dst = net_find_bound_socket_locked(s->domain, s->type, dst_addr, dst_len);
    }
    if (!dst) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return dst_addr ? -ECONNREFUSED : -EDESTADDRREQ;
    }
    if (dst->rx_count >= NET_MAX_QUEUE && !dontwait) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return net_enqueue_msg_blocking(dst, buf, len, s->local, s->local_len,
                                        0, s->send_timeout_ticks);
    }
    int r = net_enqueue_msg_locked(dst, buf, len, s->local, s->local_len);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return r;
}

int net_recvfrom(int gfd, void *buf, size_t len, int flags,
                 void *addr, size_t *addrlen) {
    net_socket_t *s = net_socket_from_file(gfd);
    if (!s) return -ENOTSOCK;
    if (s->domain == AF_ALG)
        return net_alg_socket_recv(s, buf, len);
    int dontwait = (flags & MSG_DONTWAIT) != 0;
    uint64_t start = timer_get_ticks();
    for (;;) {
        a20_lwip_poll();
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        int r = net_dequeue_msg_locked(s, buf, len, addr, addrlen);
        if (r > 0 && s->type == SOCK_STREAM) {
            size_t total = (size_t)r;
            while (total < len && s->rx_head) {
                int nr = net_dequeue_msg_locked(s, (char *)buf + total, len - total, NULL, NULL);
                if (nr <= 0)
                    break;
                total += (size_t)nr;
            }
            r = (int)total;
        }
        if (r != -EAGAIN || s->nonblock || dontwait) {
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
