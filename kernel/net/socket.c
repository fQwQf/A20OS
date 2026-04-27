#include "net/socket.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/timer.h"
#include "drv/virtio_net.h"
#include "net/lwip_stack.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#define NET_MAX_SOCKETS 128
#define NET_MAX_PAYLOAD 2048
#define NET_MAX_QUEUE   128
#define NET_WAIT_TICKS  MS_TO_TICKS(50)
#define NET_CONNECT_TIMEOUT_TICKS MS_TO_TICKS(10000)

typedef struct net_msg {
    struct net_msg *next;
    size_t len;
    uint8_t addr[NET_SOCKADDR_MAX];
    size_t addrlen;
    uint8_t data[NET_MAX_PAYLOAD];
} net_msg_t;

typedef struct net_socket {
    int domain;
    int type;
    int protocol;
    int nonblock;
    int closed;
    int bound;
    int connected;
    int listening;
    uint8_t local[NET_SOCKADDR_MAX];
    size_t local_len;
    uint8_t peer_addr[NET_SOCKADDR_MAX];
    size_t peer_len;
    struct net_socket *peer;
    net_msg_t *rx_head;
    net_msg_t *rx_tail;
    int rx_count;
    task_t *waiter;
    struct udp_pcb *udp;
    struct raw_pcb *raw;
    struct tcp_pcb *tcp;
    int tcp_connecting;
    int tcp_err;
    struct net_socket *accept_next;
    struct net_socket *accept_head;
    struct net_socket *accept_tail;
    int accept_count;
} net_socket_t;

static spinlock_t g_net_lock = SPINLOCK_INIT;
static net_socket_t *g_sockets[NET_MAX_SOCKETS];
static uint16_t g_next_ephemeral = 49152;
static vfile_ops_t g_net_ops;

static uint16_t net_htons(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static uint16_t net_ntohs(uint16_t x) {
    return net_htons(x);
}

static int sockaddr_family(const void *addr, size_t len) {
    if (!addr || len < sizeof(uint16_t))
        return -EINVAL;
    return *(const uint16_t *)addr;
}

static int sockaddr_port(const void *addr, size_t len, uint16_t *port) {
    if (!addr || len < sizeof(net_sockaddr_in_t) || !port)
        return -EINVAL;
    int family = sockaddr_family(addr, len);
    if (family != AF_INET && family != AF_INET6)
        return -EAFNOSUPPORT;
    *port = ((const net_sockaddr_in_t *)addr)->sin_port;
    return 0;
}

static void sockaddr_loopback(net_socket_t *s, uint16_t port) {
    net_sockaddr_in_t in;
    memset(&in, 0, sizeof(in));
    in.sin_family = (uint16_t)s->domain;
    in.sin_port = port;
    in.sin_addr = 0x0100007fU;
    memcpy(s->local, &in, sizeof(in));
    s->local_len = sizeof(in);
    s->bound = 1;
}

static int sockaddr_to_lwip_ip(const void *addr, size_t len, ip_addr_t *ip, uint16_t *port) {
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

static int lwip_ip_to_sockaddr(const ip_addr_t *ip, uint16_t port,
                               uint8_t out[NET_SOCKADDR_MAX], size_t *outlen) {
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

static uint16_t alloc_ephemeral_port_locked(void) {
    uint16_t p = g_next_ephemeral++;
    if (g_next_ephemeral < 49152)
        g_next_ephemeral = 49152;
    return net_htons(p);
}

static int register_socket_locked(net_socket_t *s) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            g_sockets[i] = s;
            return 0;
        }
    }
    return -ENFILE;
}

static void unregister_socket_locked(net_socket_t *s) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (g_sockets[i] == s) {
            g_sockets[i] = NULL;
            break;
        }
    }
}

static net_socket_t *file_socket(int gfd) {
    vfile_t *vf = vfs_get_file(gfd);
    if (!vf || vf->ops != &g_net_ops || !vf->priv)
        return NULL;
    return (net_socket_t *)vf->priv;
}

static void block_on_socket_locked(net_socket_t *s, task_t *cur) {
    s->waiter = cur;
    cur->wake_time = timer_get_ticks() + NET_WAIT_TICKS;
    cur->state = PROC_BLOCKED;
}

static void clear_socket_waiter(net_socket_t *s, task_t *cur) {
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->waiter == cur)
        s->waiter = NULL;
    cur->wake_time = 0;
    spin_unlock_irqrestore(&g_net_lock, irq);
}

static net_socket_t *find_bound_socket_locked(int domain, int type,
                                              const void *addr, size_t addrlen) {
    uint16_t port = 0;
    if (sockaddr_port(addr, addrlen, &port) < 0)
        return NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = g_sockets[i];
        if (!s || !s->bound || s->domain != domain || s->type != type)
            continue;
        uint16_t s_port = 0;
        if (sockaddr_port(s->local, s->local_len, &s_port) == 0 && s_port == port)
            return s;
    }
    return NULL;
}

static int enqueue_msg_locked(net_socket_t *dst, const void *buf, size_t len,
                              const void *addr, size_t addrlen) {
    if (!dst || dst->closed)
        return -ENOTCONN;
    if (len > NET_MAX_PAYLOAD)
        return -EMSGSIZE;
    if (dst->rx_count >= NET_MAX_QUEUE)
        return -EAGAIN;

    net_msg_t *m = (net_msg_t *)kmalloc(sizeof(net_msg_t));
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
    if (dst->waiter && dst->waiter->state == PROC_BLOCKED)
        proc_make_ready(dst->waiter);
    return (int)len;
}

static void lwip_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return;

    uint8_t from[NET_SOCKADDR_MAX];
    size_t fromlen = 0;
    lwip_ip_to_sockaddr(addr, port, from, &fromlen);

    uint8_t data[NET_MAX_PAYLOAD];
    size_t len = p->tot_len;
    if (len > sizeof(data))
        len = sizeof(data);
    pbuf_copy_partial(p, data, (u16_t)len, 0);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    enqueue_msg_locked(s, data, len, fromlen ? from : NULL, fromlen);
    spin_unlock_irqrestore(&g_net_lock, irq);
    pbuf_free(p);
}

static u8_t lwip_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr) {
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return 0;

    uint8_t from[NET_SOCKADDR_MAX];
    size_t fromlen = 0;
    lwip_ip_to_sockaddr(addr, 0, from, &fromlen);

    uint8_t data[NET_MAX_PAYLOAD];
    size_t len = p->tot_len;
    if (len > sizeof(data))
        len = sizeof(data);
    pbuf_copy_partial(p, data, (u16_t)len, 0);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    enqueue_msg_locked(s, data, len, fromlen ? from : NULL, fromlen);
    spin_unlock_irqrestore(&g_net_lock, irq);
    pbuf_free(p);
    return 1;
}

static err_t lwip_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
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

static err_t lwip_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
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

    int chunks = (int)((p->tot_len + NET_MAX_PAYLOAD - 1) / NET_MAX_PAYLOAD);
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->rx_count + chunks > NET_MAX_QUEUE) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return ERR_MEM;
    }

    uint8_t data[NET_MAX_PAYLOAD];
    size_t off = 0;
    while (off < p->tot_len) {
        size_t n = p->tot_len - off;
        if (n > sizeof(data))
            n = sizeof(data);
        pbuf_copy_partial(p, data, (u16_t)n, (u16_t)off);
        int r = enqueue_msg_locked(s, data, n, NULL, 0);
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

static void lwip_tcp_err_cb(void *arg, err_t err) {
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

static err_t lwip_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || !arg || !newpcb)
        return ERR_VAL;

    net_socket_t *listener = (net_socket_t *)arg;
    net_socket_t *child = (net_socket_t *)kmalloc(sizeof(net_socket_t));
    if (!child)
        return ERR_MEM;
    memset(child, 0, sizeof(*child));
    child->domain = AF_INET;
    child->type = SOCK_STREAM;
    child->tcp = newpcb;
    child->connected = 1;
    child->bound = 1;

    lwip_ip_to_sockaddr(&newpcb->local_ip, newpcb->local_port,
                        child->local, &child->local_len);
    lwip_ip_to_sockaddr(&newpcb->remote_ip, newpcb->remote_port,
                        child->peer_addr, &child->peer_len);

    tcp_arg(newpcb, child);
    tcp_recv(newpcb, lwip_tcp_recv_cb);
    tcp_err(newpcb, lwip_tcp_err_cb);

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (listener->closed || listener->accept_count >= NET_MAX_QUEUE) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        tcp_abort(newpcb);
        kfree(child);
        return ERR_ABRT;
    }
    if (listener->accept_tail)
        listener->accept_tail->accept_next = child;
    else
        listener->accept_head = child;
    listener->accept_tail = child;
    listener->accept_count++;
    if (listener->waiter && listener->waiter->state == PROC_BLOCKED)
        proc_make_ready(listener->waiter);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return ERR_OK;
}

static int dequeue_msg_locked(net_socket_t *s, void *buf, size_t len,
                              void *addr, size_t *addrlen) {
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
    kfree(m);
    return (int)n;
}

static int net_vfile_read(vfile_t *vf, char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    for (;;) {
        a20_lwip_poll();
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        int r = dequeue_msg_locked(s, buf, count, NULL, NULL);
        if (r != -EAGAIN || s->nonblock) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return r;
        }
        task_t *cur = proc_current();
        if (!cur) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EAGAIN;
        }
        block_on_socket_locked(s, cur);
        spin_unlock_irqrestore(&g_net_lock, irq);
        sched();
        clear_socket_waiter(s, cur);
    }
}

static int net_vfile_write(vfile_t *vf, const char *buf, size_t count) {
    net_socket_t *s = (net_socket_t *)vf->priv;
    if (!s)
        return -ENOTSOCK;
    if (count > NET_MAX_PAYLOAD)
        return -EMSGSIZE;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        sockaddr_loopback(s, alloc_ephemeral_port_locked());
    net_socket_t *dst = s->peer;
    if (!dst && s->connected)
        dst = find_bound_socket_locked(s->domain, s->type, s->peer_addr, s->peer_len);
    if (!dst) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return s->connected ? -ECONNREFUSED : -EDESTADDRREQ;
    }
    int r = enqueue_msg_locked(dst, buf, count, s->local, s->local_len);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return r;
}

static long net_vfile_lseek(vfile_t *vf, long offset, int whence) {
    (void)vf; (void)offset; (void)whence;
    return -ESPIPE;
}

static int net_vfile_close(vfile_t *vf) {
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
    if (s->tcp) {
        tcp_arg(s->tcp, NULL);
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
        if (tcp_close(s->tcp) != ERR_OK)
            tcp_abort(s->tcp);
        s->tcp = NULL;
    }
    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    unregister_socket_locked(s);
    if (s->peer && s->peer->peer == s)
        s->peer->peer = NULL;
    s->closed = 1;
    net_msg_t *m = s->rx_head;
    s->rx_head = s->rx_tail = NULL;
    net_socket_t *accepted = s->accept_head;
    s->accept_head = s->accept_tail = NULL;
    s->accept_count = 0;
    spin_unlock_irqrestore(&g_net_lock, flags);

    while (m) {
        net_msg_t *next = m->next;
        kfree(m);
        m = next;
    }
    while (accepted) {
        net_socket_t *next = accepted->accept_next;
        if (accepted->tcp) {
            tcp_arg(accepted->tcp, NULL);
            tcp_recv(accepted->tcp, NULL);
            tcp_err(accepted->tcp, NULL);
            if (tcp_close(accepted->tcp) != ERR_OK)
                tcp_abort(accepted->tcp);
        }
        kfree(accepted);
        accepted = next;
    }
    kfree(s);
    vf->priv = NULL;
    return 0;
}

static vfile_ops_t g_net_ops = {
    .read = net_vfile_read,
    .write = net_vfile_write,
    .lseek = net_vfile_lseek,
    .close = net_vfile_close,
};

void net_init(void) {
    spin_init(&g_net_lock);
    memset(g_sockets, 0, sizeof(g_sockets));
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
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
        return -EAFNOSUPPORT;
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM && base_type != SOCK_RAW)
        return -EPROTOTYPE;
    if (base_type == SOCK_RAW && (domain != AF_INET || protocol < 0 || protocol > 255))
        return -EPROTONOSUPPORT;

    net_socket_t *s = (net_socket_t *)kmalloc(sizeof(net_socket_t));
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!s || !vf) {
        if (s) kfree(s);
        if (vf) kfree(vf);
        return -ENOMEM;
    }
    memset(s, 0, sizeof(*s));
    memset(vf, 0, sizeof(*vf));
    s->domain = domain;
    s->type = base_type;
    s->protocol = protocol;
    s->nonblock = (type & SOCK_NONBLOCK) != 0;
    if ((domain == AF_INET || domain == AF_INET6) && base_type == SOCK_DGRAM) {
        s->udp = udp_new_ip_type(domain == AF_INET6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
        if (!s->udp) {
            kfree(s);
            kfree(vf);
            return -ENOMEM;
        }
        udp_recv(s->udp, lwip_udp_recv_cb, s);
    } else if (domain == AF_INET && base_type == SOCK_RAW) {
        s->raw = raw_new_ip_type(IPADDR_TYPE_V4, (u8_t)protocol);
        if (!s->raw) {
            kfree(s);
            kfree(vf);
            return -ENOMEM;
        }
        raw_recv(s->raw, lwip_raw_recv_cb, s);
    } else if (domain == AF_INET && base_type == SOCK_STREAM) {
        s->tcp = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!s->tcp) {
            kfree(s);
            kfree(vf);
            return -ENOMEM;
        }
        tcp_arg(s->tcp, s);
        tcp_recv(s->tcp, lwip_tcp_recv_cb);
        tcp_err(s->tcp, lwip_tcp_err_cb);
    }

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    int r = register_socket_locked(s);
    spin_unlock_irqrestore(&g_net_lock, flags);
    if (r < 0) {
        if (s->udp) udp_remove(s->udp);
        if (s->raw) raw_remove(s->raw);
        if (s->tcp) tcp_abort(s->tcp);
        kfree(s);
        kfree(vf);
        return r;
    }

    vf->flags = type;
    vf->ref_count = 1;
    vf->ops = &g_net_ops;
    vf->priv = s;
    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        net_vfile_close(vf);
        kfree(vf);
        return gfd;
    }
    return gfd;
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
    net_socket_t *sa = file_socket(a);
    net_socket_t *sb = file_socket(b);
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
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    int family = sockaddr_family(addr, addrlen);
    if (family < 0) return family;
    if (family != s->domain) return -EAFNOSUPPORT;
    if (addrlen > NET_SOCKADDR_MAX) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        find_bound_socket_locked(s->domain, s->type, addr, addrlen)) {
        spin_unlock_irqrestore(&g_net_lock, flags);
        return -EADDRINUSE;
    }
    memcpy(s->local, addr, addrlen);
    s->local_len = addrlen;
    s->bound = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    if (s->udp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0) return r;
        err_t e = udp_bind(s->udp, &ip, port);
        if (e != ERR_OK) return -EADDRINUSE;
    } else if (s->raw && s->domain == AF_INET) {
        ip_addr_t ip;
        int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0) return r;
        err_t e = raw_bind(s->raw, &ip);
        if (e != ERR_OK) return -EADDRINUSE;
    } else if (s->tcp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0) return r;
        err_t e = tcp_bind(s->tcp, &ip, port);
        if (e != ERR_OK) return -EADDRINUSE;
    }
    return 0;
}

int net_connect(int gfd, const void *addr, size_t addrlen) {
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    if (!addr || addrlen > NET_SOCKADDR_MAX) return -EINVAL;
    int family = sockaddr_family(addr, addrlen);
    if (family != s->domain) return -EAFNOSUPPORT;

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        sockaddr_loopback(s, alloc_ephemeral_port_locked());
    memcpy(s->peer_addr, addr, addrlen);
    s->peer_len = addrlen;
    s->connected = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    if (s->udp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0) return r;
        err_t e = udp_connect(s->udp, &ip, port);
        if (e != ERR_OK) return -ENETUNREACH;
    } else if (s->raw && s->domain == AF_INET) {
        ip_addr_t ip;
        int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
        if (r < 0) return r;
        err_t e = raw_connect(s->raw, &ip);
        if (e != ERR_OK) return -ENETUNREACH;
    } else if (s->tcp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
        if (r < 0) return r;
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
            uint64_t irq = spin_lock_irqsave(&g_net_lock);
            block_on_socket_locked(s, cur);
            spin_unlock_irqrestore(&g_net_lock, irq);
            a20_lwip_poll();
            sched();
            clear_socket_waiter(s, cur);
        }
        if (s->tcp_err != ERR_OK)
            return -ECONNREFUSED;
    }
    return 0;
}

int net_listen(int gfd, int backlog) {
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    if (!s->tcp || s->domain != AF_INET)
        return -EOPNOTSUPP;
    if (s->listening)
        return 0;
    if (backlog <= 0)
        backlog = 1;
    if (backlog > NET_MAX_QUEUE)
        backlog = NET_MAX_QUEUE;

    err_t err = ERR_OK;
    struct tcp_pcb *listener = tcp_listen_with_backlog_and_err(s->tcp, (u8_t)backlog, &err);
    if (!listener || err != ERR_OK)
        return -EADDRINUSE;
    s->tcp = listener;
    s->listening = 1;
    tcp_arg(s->tcp, s);
    tcp_accept(s->tcp, lwip_tcp_accept_cb);
    return 0;
}

int net_accept(int gfd, void *addr, size_t *addrlen, int flags) {
    (void)flags;
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    if (!s->listening || !s->tcp)
        return -EINVAL;

    net_socket_t *child = NULL;
    for (;;) {
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        child = s->accept_head;
        if (child) {
            s->accept_head = child->accept_next;
            if (!s->accept_head)
                s->accept_tail = NULL;
            s->accept_count--;
            child->accept_next = NULL;
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
        block_on_socket_locked(s, cur);
        spin_unlock_irqrestore(&g_net_lock, irq);
        a20_lwip_poll();
        sched();
        clear_socket_waiter(s, cur);
    }

    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) {
        tcp_abort(child->tcp);
        kfree(child);
        return -ENOMEM;
    }
    memset(vf, 0, sizeof(*vf));

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    int r = register_socket_locked(child);
    spin_unlock_irqrestore(&g_net_lock, irq);
    if (r < 0) {
        tcp_abort(child->tcp);
        kfree(child);
        kfree(vf);
        return r;
    }
    tcp_backlog_accepted(child->tcp);

    if (addr && addrlen && *addrlen > 0) {
        size_t n = child->peer_len < *addrlen ? child->peer_len : *addrlen;
        memcpy(addr, child->peer_addr, n);
        *addrlen = n;
    }

    vf->flags = s->nonblock ? SOCK_NONBLOCK : 0;
    vf->ref_count = 1;
    vf->ops = &g_net_ops;
    vf->priv = child;
    int newfd = vfs_alloc_fd(vf);
    if (newfd < 0) {
        net_vfile_close(vf);
        kfree(vf);
        return newfd;
    }
    return newfd;
}

int net_sendto(int gfd, const void *buf, size_t len, int flags,
               const void *addr, size_t addrlen) {
    (void)flags;
    net_socket_t *s = (gfd >= 0) ? file_socket(gfd) : NULL;
    if (!s) return -ENOTSOCK;
    if (len > NET_MAX_PAYLOAD) return -EMSGSIZE;
    if (s->udp && s->domain == AF_INET) {
        if (!s->bound) {
            uint16_t port = alloc_ephemeral_port_locked();
            sockaddr_loopback(s, port);
            ip_addr_t any;
            ip_addr_set_zero_ip4(&any);
            udp_bind(s->udp, &any, net_ntohs(port));
        }
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (!p) return -ENOMEM;
        pbuf_take(p, buf, (u16_t)len);
        err_t e;
        if (addr) {
            ip_addr_t ip;
            uint16_t port = 0;
            int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, &port);
            if (r < 0) { pbuf_free(p); return r; }
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
    if (s->raw && s->domain == AF_INET) {
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (!p) return -ENOMEM;
        pbuf_take(p, buf, (u16_t)len);
        err_t e;
        if (addr) {
            ip_addr_t ip;
            int r = sockaddr_to_lwip_ip(addr, addrlen, &ip, NULL);
            if (r < 0) { pbuf_free(p); return r; }
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
    if (s->tcp && s->domain == AF_INET) {
        if (!s->connected || s->closed)
            return -ENOTCONN;
        if (len > tcp_sndbuf(s->tcp))
            return -EAGAIN;
        err_t e = tcp_write(s->tcp, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
        if (e == ERR_OK)
            e = tcp_output(s->tcp);
        a20_lwip_poll();
        return e == ERR_OK ? (int)len : -EIO;
    }

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        sockaddr_loopback(s, alloc_ephemeral_port_locked());

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
        dst = find_bound_socket_locked(s->domain, s->type, dst_addr, dst_len);
    }
    if (!dst) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return dst_addr ? -ECONNREFUSED : -EDESTADDRREQ;
    }
    int r = enqueue_msg_locked(dst, buf, len, s->local, s->local_len);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return r;
}

int net_recvfrom(int gfd, void *buf, size_t len, int flags,
                 void *addr, size_t *addrlen) {
    (void)flags;
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    for (;;) {
        a20_lwip_poll();
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        int r = dequeue_msg_locked(s, buf, len, addr, addrlen);
        if (r > 0 && s->type == SOCK_STREAM) {
            size_t total = (size_t)r;
            while (total < len && s->rx_head) {
                int nr = dequeue_msg_locked(s, (char *)buf + total, len - total, NULL, NULL);
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
        block_on_socket_locked(s, cur);
        spin_unlock_irqrestore(&g_net_lock, irq);
        sched();
        clear_socket_waiter(s, cur);
    }
}

int net_getsockname(int gfd, void *addr, size_t *addrlen) {
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    if (!addr || !addrlen) return -EFAULT;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        sockaddr_loopback(s, alloc_ephemeral_port_locked());
    size_t n = s->local_len < *addrlen ? s->local_len : *addrlen;
    memcpy(addr, s->local, n);
    *addrlen = n;
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}

int net_getpeername(int gfd, void *addr, size_t *addrlen) {
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    if (!s->connected) return -ENOTCONN;
    size_t n = s->peer_len < *addrlen ? s->peer_len : *addrlen;
    memcpy(addr, s->peer_addr, n);
    *addrlen = n;
    return 0;
}

int net_setsockopt(int gfd, int level, int optname, const void *optval, size_t optlen) {
    (void)optval; (void)optlen;
    if (!file_socket(gfd)) return -ENOTSOCK;
    if (level == SOL_SOCKET && (optname == SO_TYPE || optname == SO_ERROR))
        return 0;
    return -EOPNOTSUPP;
}

int net_getsockopt(int gfd, int level, int optname, void *optval, size_t *optlen) {
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    if (!optval || !optlen || *optlen < sizeof(int)) return -EINVAL;
    int val = 0;
    if (level == SOL_SOCKET && optname == SO_TYPE)
        val = s->type;
    else if (level == SOL_SOCKET && optname == SO_ERROR)
        val = 0;
    else
        return -EOPNOTSUPP;
    memcpy(optval, &val, sizeof(val));
    *optlen = sizeof(val);
    return 0;
}

int net_shutdown(int gfd, int how) {
    (void)how;
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    s->closed = 1;
    if (s->waiter && s->waiter->state == PROC_BLOCKED)
        proc_make_ready(s->waiter);
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}

int net_set_nonblock(int gfd, int nonblock) {
    net_socket_t *s = file_socket(gfd);
    if (!s)
        return -ENOTSOCK;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    s->nonblock = nonblock ? 1 : 0;
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}
