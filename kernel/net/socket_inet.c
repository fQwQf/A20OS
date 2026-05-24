#include "net/socket_internal.h"
#include "net/lwip_stack.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/slab.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

static uint16_t g_next_ephemeral = 49152;

static uint16_t net_htons(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

uint16_t net_ntohs(uint16_t x)
{
    return net_htons(x);
}

uint16_t net_alloc_ephemeral_port_locked(void)
{
    uint16_t p = g_next_ephemeral++;
    if (g_next_ephemeral < 49152)
        g_next_ephemeral = 49152;
    return net_htons(p);
}

void net_sockaddr_loopback(net_socket_t *s, uint16_t port)
{
    net_sockaddr_in_t in;
    memset(&in, 0, sizeof(in));
    in.sin_family = (uint16_t)s->domain;
    in.sin_port = port;
    in.sin_addr = 0x0100007fU;
    memcpy(s->local, &in, sizeof(in));
    s->local_len = sizeof(in);
    s->bound = 1;
}

int net_sockaddr_port(const void *addr, size_t len, uint16_t *port)
{
    if (!addr || len < sizeof(net_sockaddr_in_t) || !port)
        return -EINVAL;
    int family = *(const uint16_t *)addr;
    if (family != AF_INET && family != AF_INET6)
        return -EAFNOSUPPORT;
    *port = ((const net_sockaddr_in_t *)addr)->sin_port;
    return 0;
}

void net_sockaddr_set_port(void *addr, size_t len, uint16_t port)
{
    if (!addr || len < sizeof(net_sockaddr_in_t))
        return;
    net_sockaddr_in_t *in = (net_sockaddr_in_t *)addr;
    if (in->sin_family == AF_INET || in->sin_family == AF_INET6)
        in->sin_port = port;
}

int net_sockaddr_in_local(const net_sockaddr_in_t *in)
{
    if (!in)
        return 0;
    uint32_t addr = in->sin_addr;
    if (addr == 0 || addr == 0x0100007fU || addr == 0x0f02000aU)
        return 1;
    return 0;
}

static int net_inet_domains_overlap(int a, int b)
{
    if (a == b)
        return 1;
    return (a == AF_INET && b == AF_INET6) || (a == AF_INET6 && b == AF_INET);
}

static int net_sockaddr_port_equal(const void *a, size_t alen,
                                   const void *b, size_t blen)
{
    uint16_t ap = 0;
    uint16_t bp = 0;
    return net_sockaddr_port(a, alen, &ap) == 0 &&
           net_sockaddr_port(b, blen, &bp) == 0 &&
           ap == bp;
}

static int net_sockaddr_is_local_target(const void *addr, size_t len)
{
    if (!addr || len < sizeof(net_sockaddr_in_t))
        return 0;
    int family = *(const uint16_t *)addr;
    if (family == AF_INET)
        return net_sockaddr_in_local((const net_sockaddr_in_t *)addr);
    if (family == AF_INET6 && len >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        int all_zero = 1;
        for (size_t i = 0; i < sizeof(in6->sin6_addr); i++) {
            if (in6->sin6_addr[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero)
            return 1;
        for (size_t i = 0; i < 15; i++) {
            if (in6->sin6_addr[i] != 0)
                return 0;
        }
        return in6->sin6_addr[15] == 1;
    }
    return 0;
}

static net_socket_t *net_find_stream_listener_locked(net_socket_t *s,
                                                     uint16_t port)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *cand = g_sockets[i];
        if (!cand || !cand->bound || !cand->listening ||
            cand->type != SOCK_STREAM)
            continue;
        if (!net_inet_domains_overlap(cand->domain, s->domain))
            continue;
        uint16_t cand_port = 0;
        if (net_sockaddr_port(cand->local, cand->local_len, &cand_port) == 0 &&
            cand_port == port)
            return cand;
    }
    return NULL;
}

static net_socket_t *net_find_udp_dst_locked(net_socket_t *src,
                                             const void *dst_addr,
                                             size_t dst_len)
{
    if (!src || !dst_addr)
        return NULL;
    uint16_t dst_port = 0;
    if (net_sockaddr_port(dst_addr, dst_len, &dst_port) < 0)
        return NULL;

    net_socket_t *fallback = NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *cand = g_sockets[i];
        if (!cand || cand == src || !cand->bound || cand->type != SOCK_DGRAM)
            continue;
        if (!net_inet_domains_overlap(cand->domain, src->domain))
            continue;
        uint16_t cand_port = 0;
        if (net_sockaddr_port(cand->local, cand->local_len, &cand_port) < 0 ||
            cand_port != dst_port)
            continue;
        if (cand->connected) {
            if (net_sockaddr_port_equal(cand->peer_addr, cand->peer_len,
                                        src->local, src->local_len))
                return cand;
            continue;
        }
        if (!fallback)
            fallback = cand;
    }
    return fallback;
}

int net_sockaddr_to_lwip_ip(const void *addr, size_t len,
                            ip_addr_t *ip, uint16_t *port)
{
    if (!addr || !ip || len < sizeof(net_sockaddr_in_t))
        return -EINVAL;
    const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)addr;
    if (in->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    ip_addr_set_ip4_u32(ip, in->sin_addr);
    if (port)
        *port = net_ntohs(in->sin_port);
    return 0;
}

int net_lwip_ip_to_sockaddr(const ip_addr_t *ip, uint16_t port,
                            uint8_t out[NET_SOCKADDR_MAX], size_t *outlen)
{
    if (!out || !outlen || !IP_IS_V4(ip))
        return -EINVAL;
    net_sockaddr_in_t in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = net_htons(port);
    in.sin_addr = ip_2_ip4(ip)->addr;
    memcpy(out, &in, sizeof(in));
    *outlen = sizeof(in);
    return 0;
}

static void lwip_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return;

    uint8_t from[NET_SOCKADDR_MAX];
    size_t fromlen = 0;
    net_lwip_ip_to_sockaddr(addr, port, from, &fromlen);

    size_t len = p->tot_len;
    if (len > NET_MAX_PAYLOAD)
        len = NET_MAX_PAYLOAD;
    uint8_t *data = (uint8_t *)kmalloc(len ? len : 1);
    if (!data) {
        pbuf_free(p);
        return;
    }
    pbuf_copy_partial(p, data, (u16_t)len, 0);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    net_enqueue_msg_locked(s, data, len, fromlen ? from : NULL, fromlen);
    spin_unlock_irqrestore(&g_net_lock, irq);
    kfree(data);
    pbuf_free(p);
}

static u8_t lwip_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return 0;

    uint8_t from[NET_SOCKADDR_MAX];
    size_t fromlen = 0;
    net_lwip_ip_to_sockaddr(addr, 0, from, &fromlen);

    size_t len = p->tot_len;
    if (len > NET_MAX_PAYLOAD)
        len = NET_MAX_PAYLOAD;
    uint8_t *data = (uint8_t *)kmalloc(len ? len : 1);
    if (!data) {
        pbuf_free(p);
        return 0;
    }
    pbuf_copy_partial(p, data, (u16_t)len, 0);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    net_enqueue_msg_locked(s, data, len, fromlen ? from : NULL, fromlen);
    spin_unlock_irqrestore(&g_net_lock, irq);
    kfree(data);
    pbuf_free(p);
    return 1;
}

static err_t lwip_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    s->tcp_connecting = 0;
    s->tcp_err = err;
    if (err == ERR_OK)
        s->connected = 1;
    if (s->waiter && s->waiter->state == PROC_BLOCKED)
        proc_make_ready(s->waiter);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return ERR_OK;
}

static err_t lwip_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                              err_t err)
{
    (void)err;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;
    if (!p) {
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        s->closed = 1;
        if (s->waiter && s->waiter->state == PROC_BLOCKED)
            proc_make_ready(s->waiter);
        spin_unlock_irqrestore(&g_net_lock, irq);
        return ERR_OK;
    }

    int chunks = (int)((p->tot_len + NET_MAX_STREAM_PAYLOAD - 1) / NET_MAX_STREAM_PAYLOAD);
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->rx_count + chunks > NET_MAX_QUEUE) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return ERR_MEM;
    }

    uint8_t data[NET_MAX_STREAM_PAYLOAD];
    size_t off = 0;
    while (off < p->tot_len) {
        size_t n = p->tot_len - off;
        if (n > sizeof(data))
            n = sizeof(data);
        pbuf_copy_partial(p, data, (u16_t)n, (u16_t)off);
        int r = net_enqueue_msg_locked(s, data, n, NULL, 0);
        if (r < 0) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return ERR_MEM;
        }
        off += n;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void lwip_tcp_err_cb(void *arg, err_t err)
{
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    s->tcp = NULL;
    s->tcp_connecting = 0;
    s->tcp_err = err;
    s->closed = 1;
    if (s->waiter && s->waiter->state == PROC_BLOCKED)
        proc_make_ready(s->waiter);
    spin_unlock_irqrestore(&g_net_lock, irq);
}

static err_t lwip_tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->waiter && s->waiter->state == PROC_BLOCKED)
        proc_make_ready(s->waiter);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return ERR_OK;
}

void net_tcp_close_pcb(net_socket_t *s)
{
    if (!s || !s->tcp)
        return;
    tcp_arg(s->tcp, NULL);
    if (s->listening) {
        tcp_accept(s->tcp, NULL);
    } else {
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
        tcp_sent(s->tcp, NULL);
    }
    if (tcp_close(s->tcp) != ERR_OK)
        tcp_abort(s->tcp);
    s->tcp = NULL;
}

void net_tcp_drop_pcb(net_socket_t *s)
{
    if (!s || !s->tcp)
        return;
    tcp_arg(s->tcp, NULL);
    tcp_recv(s->tcp, NULL);
    tcp_err(s->tcp, NULL);
    tcp_sent(s->tcp, NULL);
    if (tcp_close(s->tcp) != ERR_OK)
        tcp_abort(s->tcp);
    s->tcp = NULL;
}

int net_inet_socket_init(net_socket_t *s)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;

    if (s->type == SOCK_DGRAM) {
        s->udp = udp_new_ip_type(s->domain == AF_INET6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
        if (!s->udp)
            return -ENOMEM;
        udp_recv(s->udp, lwip_udp_recv_cb, s);
        return 0;
    }
    if (s->domain == AF_INET && s->type == SOCK_RAW) {
        s->raw = raw_new_ip_type(IPADDR_TYPE_V4, (u8_t)s->protocol);
        if (!s->raw)
            return -ENOMEM;
        raw_recv(s->raw, lwip_raw_recv_cb, s);
        return 0;
    }
    if (s->domain == AF_INET && s->type == SOCK_STREAM) {
        s->tcp = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!s->tcp)
            return -ENOMEM;
        if (s->tcp_nodelay)
            tcp_nagle_disable(s->tcp);
        if (s->keepalive)
            s->tcp->so_options |= SOF_KEEPALIVE;
        if (s->keep_idle > 0)
            s->tcp->keep_idle = (u32_t)s->keep_idle * 1000U;
        if (s->keep_intvl > 0)
            s->tcp->keep_intvl = (u32_t)s->keep_intvl * 1000U;
        if (s->keep_cnt > 0)
            s->tcp->keep_cnt = (u32_t)s->keep_cnt;
        tcp_arg(s->tcp, s);
        tcp_recv(s->tcp, lwip_tcp_recv_cb);
        tcp_err(s->tcp, lwip_tcp_err_cb);
        tcp_sent(s->tcp, lwip_tcp_sent_cb);
    }
    return 0;
}

void net_inet_socket_destroy(net_socket_t *s)
{
    if (!s)
        return;
    if (s->udp) {
        udp_remove(s->udp);
        s->udp = NULL;
    }
    if (s->raw) {
        raw_remove(s->raw);
        s->raw = NULL;
    }
    if (s->tcp) {
        tcp_abort(s->tcp);
        s->tcp = NULL;
    }
}

int net_inet_bind_pcb(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;
    if (s->domain == AF_INET6)
        return 0;
    if (s->udp) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        err_t e = udp_bind(s->udp, &ip, port);
        return e == ERR_OK ? 0 : -EADDRINUSE;
    }
    if (s->raw) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0)
            return r;
        err_t e = raw_bind(s->raw, &ip);
        return e == ERR_OK ? 0 : -EADDRINUSE;
    }
    if (s->tcp) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        err_t e = tcp_bind(s->tcp, &ip, port);
        return e == ERR_OK ? 0 : -EADDRINUSE;
    }
    return 0;
}

static int net_inet_connect_stream(net_socket_t *s, const void *addr, size_t addrlen,
                                   const void *connect_addr, size_t peer_len)
{
    if (!s->bound) {
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        if (!s->bound)
            net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
        spin_unlock_irqrestore(&g_net_lock, irq);
    }

    net_socket_t *child = net_socket_alloc();
    if (!child)
        return -ENOMEM;
    memset(child, 0, sizeof(*child));

    uint16_t connect_port = 0;
    net_sockaddr_port(connect_addr, peer_len, &connect_port);
    int local_target = net_sockaddr_is_local_target(connect_addr, peer_len);
    int wait_error = 0;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    net_socket_t *listener = NULL;
    uint64_t wait_deadline = timer_get_ticks() + MS_TO_TICKS(1000);
    for (;;) {
        listener = net_find_stream_listener_locked(s, connect_port);
        if (listener || !local_target || s->nonblock ||
            (int64_t)(timer_get_ticks() - wait_deadline) >= 0)
            break;
        task_t *cur = proc_current();
        if (!cur)
            break;
        if (net_task_has_unblocked_signal(cur)) {
            wait_error = -EINTR;
            break;
        }
        net_block_on_socket_locked(s, cur);
        spin_unlock_irqrestore(&g_net_lock, irq);
        sched();
        net_clear_socket_waiter(s, cur);
        irq = spin_lock_irqsave(&g_net_lock);
        if (net_task_has_unblocked_signal(cur)) {
            wait_error = -EINTR;
            break;
        }
    }
    if (wait_error) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        net_socket_free(child);
        return wait_error;
    }
    if (listener && listener->listening && listener->accept_count < NET_MAX_QUEUE) {
        child->domain = listener->domain;
        child->type = SOCK_STREAM;
        child->protocol = s->protocol;
        child->bpf_prog_fd = -1;
        child->bound = 1;
        child->connected = 1;
        memcpy(child->local, listener->local, listener->local_len);
        child->local_len = listener->local_len;
        memcpy(child->peer_addr, s->local, s->local_len);
        child->peer_len = s->local_len;
        child->peer = s;
        s->peer = child;
        s->connected = 1;
        s->local_tcp = 1;
        child->local_tcp = 1;
        int qr = net_accept_queue_push_locked(listener, child);
        if (qr < 0) {
            s->connected = 0;
            s->peer = NULL;
            net_socket_free(child);
            spin_unlock_irqrestore(&g_net_lock, irq);
            return qr;
        }
        spin_unlock_irqrestore(&g_net_lock, irq);
        net_tcp_drop_pcb(s);
        return 0;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);
    net_socket_free(child);

    if (s->domain != AF_INET || !s->tcp) {
        s->connected = 0;
        return -ECONNREFUSED;
    }

    ip_addr_t ip;
    uint16_t port = 0;
    int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
    if (r < 0)
        return r;
    if (net_sockaddr_is_local_target(addr, addrlen)) {
        s->connected = 0;
        return -ECONNREFUSED;
    }
    if (s->nonblock) {
        s->connected = 0;
        return -EINPROGRESS;
    }
    s->tcp_connecting = 1;
    s->tcp_err = ERR_INPROGRESS;
    err_t e = tcp_connect(s->tcp, &ip, port, lwip_tcp_connected_cb);
    if (e != ERR_OK) {
        s->tcp_connecting = 0;
        return -ENETUNREACH;
    }

    uint64_t deadline = timer_get_ticks() + NET_CONNECT_TIMEOUT_TICKS;
    while (s->tcp_connecting) {
        if ((int64_t)(timer_get_ticks() - deadline) >= 0) {
            tcp_arg(s->tcp, NULL);
            tcp_recv(s->tcp, NULL);
            tcp_err(s->tcp, NULL);
            tcp_sent(s->tcp, NULL);
            tcp_abort(s->tcp);
            s->tcp = NULL;
            s->tcp_connecting = 0;
            s->closed = 1;
            return -ETIMEDOUT;
        }
        task_t *cur = proc_current();
        if (!cur) {
            a20_lwip_poll();
            continue;
        }
        if (net_task_has_unblocked_signal(cur)) {
            if (s->tcp) {
                tcp_arg(s->tcp, NULL);
                tcp_recv(s->tcp, NULL);
                tcp_err(s->tcp, NULL);
                tcp_sent(s->tcp, NULL);
                tcp_abort(s->tcp);
                s->tcp = NULL;
            }
            s->tcp_connecting = 0;
            s->connected = 0;
            return -ERESTARTSYS;
        }
        uint64_t wait_irq = spin_lock_irqsave(&g_net_lock);
        net_block_on_socket_locked(s, cur);
        spin_unlock_irqrestore(&g_net_lock, wait_irq);
        sched();
        net_clear_socket_waiter(s, cur);
        if (net_task_has_unblocked_signal(cur)) {
            if (s->tcp) {
                tcp_arg(s->tcp, NULL);
                tcp_recv(s->tcp, NULL);
                tcp_err(s->tcp, NULL);
                tcp_sent(s->tcp, NULL);
                tcp_abort(s->tcp);
                s->tcp = NULL;
            }
            s->tcp_connecting = 0;
            s->connected = 0;
            return -ERESTARTSYS;
        }
    }
    if (s->tcp_err != ERR_OK) {
        s->connected = 0;
        return -ECONNREFUSED;
    }
    return 0;
}

int net_inet_connect(net_socket_t *s, const void *addr, size_t addrlen,
                     const void *connect_addr, size_t peer_len)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return 0;
    if (s->udp && s->domain == AF_INET6)
        return 0;
    if (s->udp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0)
            return r;
        err_t e = udp_connect(s->udp, &ip, port);
        return e == ERR_OK ? 0 : -ENETUNREACH;
    }
    if (s->raw && s->domain == AF_INET) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0)
            return r;
        err_t e = raw_connect(s->raw, &ip);
        return e == ERR_OK ? 0 : -ENETUNREACH;
    }
    if (s->type == SOCK_STREAM)
        return net_inet_connect_stream(s, addr, addrlen, connect_addr, peer_len);
    return 0;
}

static int net_inet_send_udp(net_socket_t *s, const void *buf, size_t len,
                             int flags, const void *addr, size_t addrlen)
{
    if (!s->bound) {
        uint16_t port = net_alloc_ephemeral_port_locked();
        if (s->domain == AF_INET6) {
            net_sockaddr_in6_t in6;
            memset(&in6, 0, sizeof(in6));
            in6.sin6_family = AF_INET6;
            in6.sin6_port = port;
            in6.sin6_addr[15] = 1;
            memcpy(s->local, &in6, sizeof(in6));
            s->local_len = sizeof(in6);
            s->bound = 1;
        } else {
            net_sockaddr_loopback(s, port);
            ip_addr_t any;
            ip_addr_set_zero_ip4(&any);
            udp_bind(s->udp, &any, net_ntohs(port));
        }
    }
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    net_socket_t *local_dst = NULL;
    const void *dst_addr = addr;
    size_t dst_len = addrlen;
    if (!dst_addr && s->connected) {
        dst_addr = s->peer_addr;
        dst_len = s->peer_len;
    }
    if (s->peer && net_socket_is_valid_locked(s->peer)) {
        local_dst = s->peer;
    } else {
        if (s->peer) s->peer = NULL;
        if (dst_addr)
            local_dst = net_find_udp_dst_locked(s, dst_addr, dst_len);
    }
    if (local_dst) {
        int dontwait = s->nonblock || ((flags & MSG_DONTWAIT) != 0);
        if (local_dst->rx_count >= NET_MAX_QUEUE && !dontwait) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return net_enqueue_msg_blocking(s, local_dst, buf, len,
                                            s->local, s->local_len,
                                            dontwait, s->send_timeout_ticks);
        }
        int rr = net_enqueue_msg_locked(local_dst, buf, len, s->local, s->local_len);
        spin_unlock_irqrestore(&g_net_lock, irq);
        return rr;
    }
    spin_unlock_irqrestore(&g_net_lock, irq);

    if (s->domain == AF_INET6)
        return dst_addr ? -ECONNREFUSED : -EDESTADDRREQ;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p)
        return -ENOMEM;
    pbuf_take(p, buf, (u16_t)len);
    err_t e;
    if (addr) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0) {
            pbuf_free(p);
            return r;
        }
        e = udp_sendto(s->udp, p, &ip, port);
    } else if (s->connected) {
        e = udp_send(s->udp, p);
    } else {
        pbuf_free(p);
        return -EDESTADDRREQ;
    }
    pbuf_free(p);
    a20_lwip_poll();
    return e == ERR_OK ? (int)len : -EIO;
}

static int net_inet_send_raw(net_socket_t *s, const void *buf, size_t len,
                             const void *addr, size_t addrlen)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p)
        return -ENOMEM;
    pbuf_take(p, buf, (u16_t)len);
    err_t e;
    if (addr) {
        ip_addr_t ip;
        int r = net_sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0) {
            pbuf_free(p);
            return r;
        }
        e = raw_sendto(s->raw, p, &ip);
    } else if (s->connected) {
        e = raw_send(s->raw, p);
    } else {
        pbuf_free(p);
        return -EDESTADDRREQ;
    }
    pbuf_free(p);
    a20_lwip_poll();
    return e == ERR_OK ? (int)len : -EIO;
}

static int net_inet_send_tcp(net_socket_t *s, const void *buf, size_t len)
{
    if (!s->connected || s->closed || s->shut_wr)
        return -ENOTCONN;
    size_t sent = 0;
    uint64_t start = timer_get_ticks();
    while (sent < len) {
        a20_lwip_poll();
        if (!s->tcp || s->closed || !s->connected)
            return sent ? (int)sent : -EPIPE;
        u16_t room = tcp_sndbuf(s->tcp);
        if (room == 0) {
            if (sent || s->nonblock)
                return sent ? (int)sent : -EAGAIN;
            task_t *cur = proc_current();
            if (!cur)
                return -EAGAIN;
            if (net_task_has_unblocked_signal(cur))
                return -ERESTARTSYS;
            if (net_socket_wait_expired(s, start, 1))
                return -EAGAIN;
            uint64_t irq = spin_lock_irqsave(&g_net_lock);
            net_block_on_socket_locked(s, cur);
            spin_unlock_irqrestore(&g_net_lock, irq);
            sched();
            net_clear_socket_waiter(s, cur);
            continue;
        }
        size_t n = len - sent;
        if (n > room)
            n = room;
        if (n > 0xffff)
            n = 0xffff;
        err_t e = tcp_write(s->tcp, (const uint8_t *)buf + sent,
                            (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK)
            return sent ? (int)sent : -EIO;
        e = tcp_output(s->tcp);
        if (e != ERR_OK)
            return sent ? (int)sent : -EIO;
        sent += n;
    }
    a20_lwip_poll();
    return (int)sent;
}

int net_inet_sendto(net_socket_t *s, const void *buf, size_t len,
                    int flags, const void *addr, size_t addrlen)
{
    if (!s || (s->domain != AF_INET && s->domain != AF_INET6))
        return -EAFNOSUPPORT;
    if (s->udp)
        return net_inet_send_udp(s, buf, len, flags, addr, addrlen);
    if (s->raw)
        return net_inet_send_raw(s, buf, len, addr, addrlen);
    if (s->tcp)
        return net_inet_send_tcp(s, buf, len);
    return -EOPNOTSUPP;
}

void net_inet_accept_child_ready(net_socket_t *s)
{
    if (s && s->tcp)
        tcp_backlog_accepted(s->tcp);
}
