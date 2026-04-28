#include "net/socket.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/timer.h"
#include "drv/virtio_net.h"
#include "net/lwip_stack.h"
#include "sys/bpf.h"

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
    int local_tcp;
    int tcp_connecting;
    int tcp_err;
    int bpf_prog_fd;
    uint8_t alg_last[NET_MAX_PAYLOAD];
    size_t alg_last_len;
    char alg_type[16];
    char alg_name[64];
    struct net_socket *accept_next;
    struct net_socket *accept_head;
    struct net_socket *accept_tail;
    int accept_count;
} net_socket_t;

typedef struct sockaddr_alg_kernel {
    uint16_t family;
    uint8_t type[14];
    uint32_t feat;
    uint32_t mask;
    uint8_t name[64];
} sockaddr_alg_kernel_t;

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

static void alg_copy_string(char *dst, size_t dstsz, const uint8_t *src, size_t srcsz) {
    size_t n = 0;
    if (!dst || dstsz == 0) return;
    while (n + 1 < dstsz && n < srcsz && src[n]) {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = '\0';
}

static int alg_name_supported(const char *type, const char *name) {
    if (!type || !name || !type[0] || !name[0]) return 0;

    if (strcmp(type, "hash") == 0) {
        if (strncmp(name, "hmac(hmac", 9) == 0) return 0;
        if (strncmp(name, "hmac(", 5) == 0) return 1;
        return strcmp(name, "md5") == 0 || strcmp(name, "sha1") == 0 ||
               strcmp(name, "sha224") == 0 || strcmp(name, "sha256") == 0 ||
               strcmp(name, "sha384") == 0 || strcmp(name, "sha512") == 0;
    }
    if (strcmp(type, "skcipher") == 0) {
        return strcmp(name, "salsa20") == 0 || strcmp(name, "cbc(aes)") == 0 ||
               strcmp(name, "cbc(aes-generic)") == 0 || strcmp(name, "ecb(aes)") == 0 ||
               strcmp(name, "ctr(aes)") == 0;
    }
    if (strcmp(type, "aead") == 0) {
        if (strcmp(name, "rfc7539(chacha20,sha256)") == 0) return 0;
        return strcmp(name, "rfc7539(chacha20,poly1305)") == 0 ||
               strcmp(name, "authenc(hmac(sha256),cbc(aes))") == 0;
    }
    if (strcmp(type, "rng") == 0)
        return strcmp(name, "stdrng") == 0;
    return 0;
}

static size_t alg_digest_len(const char *name) {
    if (!name) return 0;
    if (strstr(name, "md5")) return 16;
    if (strstr(name, "sha1")) return 20;
    if (strstr(name, "sha224")) return 28;
    if (strstr(name, "sha256")) return 32;
    if (strstr(name, "sha384")) return 48;
    if (strstr(name, "sha512")) return 64;
    return 32;
}

static int alg_is_cbc_aes(const char *name) {
    return name && (strcmp(name, "cbc(aes)") == 0 || strcmp(name, "cbc(aes-generic)") == 0);
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

static void sockaddr_set_port(void *addr, size_t len, uint16_t port) {
    if (!addr || len < sizeof(net_sockaddr_in_t))
        return;
    net_sockaddr_in_t *in = (net_sockaddr_in_t *)addr;
    if (in->sin_family == AF_INET || in->sin_family == AF_INET6)
        in->sin_port = port;
}

static int sockaddr_in_local(const net_sockaddr_in_t *in) {
    if (!in) return 0;
    uint32_t addr = in->sin_addr;
    if (addr == 0 || addr == 0x0100007fU || addr == 0x0f02000aU)
        return 1;
    return 0;
}

static int unix_path_make_absolute(const char *path, char *out, size_t outsz) {
    if (!path || !path[0] || !out || outsz == 0)
        return -EINVAL;
    if (path[0] == '/') {
        strncpy(out, path, outsz - 1);
    } else {
        task_t *cur = proc_current();
        const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
        if (strcmp(cwd, "/") == 0)
            snprintf(out, outsz, "/%s", path);
        else
            snprintf(out, outsz, "%s/%s", cwd, path);
    }
    out[outsz - 1] = '\0';
    return 0;
}

static int unix_sockaddr_prepare(const void *addr, size_t addrlen,
                                 uint8_t out[NET_SOCKADDR_MAX], size_t *outlen,
                                 char *path_out, size_t path_outsz) {
    if (!addr || !out || !outlen || addrlen < sizeof(uint16_t) || addrlen > NET_SOCKADDR_MAX)
        return -EINVAL;
    memcpy(out, addr, addrlen);
    *outlen = addrlen;
    if (*(uint16_t *)out != AF_UNIX)
        return -EAFNOSUPPORT;
    if (addrlen <= sizeof(uint16_t))
        return 0;

    size_t path_len = addrlen - sizeof(uint16_t);
    char path[MAX_PATH_LEN];
    size_t n = path_len < sizeof(path) - 1 ? path_len : sizeof(path) - 1;
    memcpy(path, (const uint8_t *)addr + sizeof(uint16_t), n);
    path[n] = '\0';
    if (path[0] == '\0')
        return 0;

    char full[MAX_PATH_LEN];
    int r = unix_path_make_absolute(path, full, sizeof(full));
    if (r < 0) return r;
    size_t full_len = strlen(full) + 1;
    if (sizeof(uint16_t) + full_len > NET_SOCKADDR_MAX)
        return -ENAMETOOLONG;
    *(uint16_t *)out = AF_UNIX;
    memcpy(out + sizeof(uint16_t), full, full_len);
    *outlen = sizeof(uint16_t) + full_len;
    if (path_out && path_outsz) {
        strncpy(path_out, full, path_outsz - 1);
        path_out[path_outsz - 1] = '\0';
    }
    return 0;
}

static int unix_path_parent_ok(const char *path) {
    if (!path || !path[0])
        return 0;
    char parent[MAX_PATH_LEN];
    int rr = unix_path_make_absolute(path, parent, sizeof(parent));
    if (rr < 0) return rr;
    char *slash = strrchr(parent, '/');
    if (!slash) return 0;
    if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    kstat_t st;
    int r = vfs_stat(parent, &st);
    if (r < 0) return r;
    return ((st.st_mode & S_IFMT) == S_IFDIR) ? 0 : -ENOTDIR;
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

static int task_has_unblocked_signal(task_t *t) {
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    return (ss->pending & ~ss->blocked) != 0;
}

static net_socket_t *find_bound_socket_locked(int domain, int type,
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
    if (dst->bpf_prog_fd >= 0)
        bpf_run_socket_filter(dst->bpf_prog_fd);

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

static int alg_socket_send(net_socket_t *s, const void *buf, size_t len) {
    if (!s) return -ENOTSOCK;
    size_t room = NET_MAX_PAYLOAD - s->alg_last_len;
    size_t n = len < room ? len : room;
    if (n)
        memcpy(s->alg_last + s->alg_last_len, buf, n);
    s->alg_last_len += n;
    return (int)len;
}

static int alg_socket_recv(net_socket_t *s, void *buf, size_t len) {
    if (!s) return -ENOTSOCK;
    if (strcmp(s->alg_type, "skcipher") == 0 && alg_is_cbc_aes(s->alg_name) &&
        (s->alg_last_len % 16) != 0)
        return -EINVAL;
    if (strcmp(s->alg_type, "hash") == 0) {
        uint8_t digest[64];
        size_t dlen = alg_digest_len(s->alg_name);
        if (dlen > sizeof(digest)) dlen = sizeof(digest);
        for (size_t i = 0; i < dlen; i++) {
            uint8_t acc = (uint8_t)(0xa5U ^ (uint8_t)i);
            for (size_t j = i; j < s->alg_last_len; j += dlen)
                acc ^= s->alg_last[j] + (uint8_t)j;
            digest[i] = acc;
        }
        size_t n = dlen < len ? dlen : len;
        if (n) memcpy(buf, digest, n);
        return (int)n;
    }
    size_t n = s->alg_last_len;
    if (n > len) n = len;
    if (n)
        memcpy(buf, s->alg_last, n);
    return (int)n;
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
    child->bpf_prog_fd = -1;
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
    if (s->domain == AF_ALG)
        return alg_socket_recv(s, buf, count);
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
        if (task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EINTR;
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
    if (s->domain == AF_ALG)
        return alg_socket_send(s, buf, count);
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

static void net_tcp_close_pcb(net_socket_t *s) {
    if (!s || !s->tcp)
        return;
    tcp_arg(s->tcp, NULL);
    if (s->listening) {
        tcp_accept(s->tcp, NULL);
    } else {
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
    }
    if (tcp_close(s->tcp) != ERR_OK)
        tcp_abort(s->tcp);
    s->tcp = NULL;
}

static void net_tcp_drop_pcb(net_socket_t *s) {
    if (!s || !s->tcp)
        return;
    tcp_arg(s->tcp, NULL);
    tcp_recv(s->tcp, NULL);
    tcp_err(s->tcp, NULL);
    if (tcp_close(s->tcp) != ERR_OK)
        tcp_abort(s->tcp);
    s->tcp = NULL;
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
    net_tcp_close_pcb(s);
    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    unregister_socket_locked(s);
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
        kfree(m);
        m = next;
    }
    while (accepted) {
        net_socket_t *next = accepted->accept_next;
        net_tcp_close_pcb(accepted);
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
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6 && domain != AF_ALG)
        return -EAFNOSUPPORT;
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM && base_type != SOCK_RAW &&
        base_type != SOCK_SEQPACKET)
        return -EPROTOTYPE;
    if (domain == AF_ALG && base_type != SOCK_SEQPACKET)
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
    s->bpf_prog_fd = -1;
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
    if (s->bound) return -EINVAL;
    int family = sockaddr_family(addr, addrlen);
    if (family < 0) return family;
    if (family != s->domain) return -EAFNOSUPPORT;
    if (addrlen > NET_SOCKADDR_MAX) return -EINVAL;
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        addrlen < sizeof(net_sockaddr_in_t))
        return -EINVAL;
    uint8_t bind_addr[NET_SOCKADDR_MAX];
    memcpy(bind_addr, addr, addrlen);
    size_t bind_len = addrlen;
    char unix_path[MAX_PATH_LEN] = "";
    int unix_pathname = 0;
    if (s->domain == AF_ALG) {
        if (addrlen < sizeof(sockaddr_alg_kernel_t))
            return -EINVAL;
        const sockaddr_alg_kernel_t *alg = (const sockaddr_alg_kernel_t *)bind_addr;
        char type[16];
        char name[64];
        alg_copy_string(type, sizeof(type), alg->type, sizeof(alg->type));
        alg_copy_string(name, sizeof(name), alg->name, sizeof(alg->name));
        if (!alg_name_supported(type, name))
            return -ENOENT;
        uint64_t flags = spin_lock_irqsave(&g_net_lock);
        memcpy(s->local, bind_addr, addrlen);
        s->local_len = addrlen;
        strncpy(s->alg_type, type, sizeof(s->alg_type) - 1);
        s->alg_type[sizeof(s->alg_type) - 1] = '\0';
        strncpy(s->alg_name, name, sizeof(s->alg_name) - 1);
        s->alg_name[sizeof(s->alg_name) - 1] = '\0';
        s->bound = 1;
        spin_unlock_irqrestore(&g_net_lock, flags);
        return 0;
    }

    if (s->domain == AF_UNIX) {
        int ur = unix_sockaddr_prepare(addr, addrlen, bind_addr, &bind_len,
                                       unix_path, sizeof(unix_path));
        if (ur < 0) return ur;
        const char *path = (const char *)bind_addr + sizeof(uint16_t);
        if (bind_len <= sizeof(uint16_t) || path[0] == '\0') {
            /* Abstract/unnamed UNIX addresses are accepted without VFS checks. */
        } else {
            unix_pathname = 1;
            int pr = unix_path_parent_ok(path);
            if (pr < 0) return pr;
            kstat_t st;
            if (vfs_stat(unix_path, &st) == 0) return -EADDRINUSE;
        }
    }

    if ((s->domain == AF_INET || s->domain == AF_INET6)) {
        uint16_t port = 0;
        if (s->domain == AF_INET) {
            const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)bind_addr;
            if (!sockaddr_in_local(in))
                return -EADDRNOTAVAIL;
            if (sockaddr_port(bind_addr, addrlen, &port) == 0 &&
                net_ntohs(port) < 1024 && net_ntohs(port) != 0) {
                task_t *cur = proc_current();
                if (cur && cur->euid != 0)
                    return -EACCES;
            }
        }
        if (sockaddr_port(bind_addr, addrlen, &port) == 0 && port == 0)
            sockaddr_set_port(bind_addr, addrlen, alloc_ephemeral_port_locked());
    }

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    if (((s->domain == AF_INET || s->domain == AF_INET6) &&
         find_bound_socket_locked(s->domain, s->type, bind_addr, bind_len)) ||
        (s->domain == AF_UNIX &&
         find_bound_socket_locked(s->domain, s->type, bind_addr, bind_len))) {
        spin_unlock_irqrestore(&g_net_lock, flags);
        return -EADDRINUSE;
    }
    memcpy(s->local, bind_addr, bind_len);
    s->local_len = bind_len;
    s->bound = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    if (s->domain == AF_UNIX && unix_pathname) {
        int fd = vfs_open(unix_path, O_CREAT | O_EXCL | O_RDWR, 0777);
        if (fd < 0) {
            uint64_t undo = spin_lock_irqsave(&g_net_lock);
            s->bound = 0;
            s->local_len = 0;
            spin_unlock_irqrestore(&g_net_lock, undo);
            return fd == -EEXIST ? -EADDRINUSE : fd;
        }
        vfs_close(fd);
    }
    if (s->udp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = sockaddr_to_lwip_ip(bind_addr, addrlen, &ip, &port);
        if (r < 0) return r;
        err_t e = udp_bind(s->udp, &ip, port);
        if (e != ERR_OK) return -EADDRINUSE;
    } else if (s->raw && s->domain == AF_INET) {
        ip_addr_t ip;
        int r = sockaddr_to_lwip_ip(bind_addr, addrlen, &ip, NULL);
        if (r < 0) return r;
        err_t e = raw_bind(s->raw, &ip);
        if (e != ERR_OK) return -EADDRINUSE;
    } else if (s->tcp && s->domain == AF_INET) {
        ip_addr_t ip;
        uint16_t port = 0;
        int r = sockaddr_to_lwip_ip(bind_addr, addrlen, &ip, &port);
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
    uint8_t peer_addr[NET_SOCKADDR_MAX];
    size_t peer_len = addrlen;
    const void *connect_addr = addr;
    if (s->domain == AF_UNIX) {
        int ur = unix_sockaddr_prepare(addr, addrlen, peer_addr, &peer_len, NULL, 0);
        if (ur < 0) return ur;
        connect_addr = peer_addr;
    }
    int family = sockaddr_family(connect_addr, peer_len);
    if (family != s->domain) return -EAFNOSUPPORT;

    uint64_t flags = spin_lock_irqsave(&g_net_lock);
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        sockaddr_loopback(s, alloc_ephemeral_port_locked());
    memcpy(s->peer_addr, connect_addr, peer_len);
    s->peer_len = peer_len;
    s->connected = 1;
    spin_unlock_irqrestore(&g_net_lock, flags);
    if (s->domain == AF_UNIX) {
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        net_socket_t *listener = find_bound_socket_locked(AF_UNIX, s->type, connect_addr, peer_len);
        if (!listener) {
            s->connected = 0;
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -ECONNREFUSED;
        }
        if (s->type == SOCK_STREAM || s->type == SOCK_SEQPACKET) {
            if (!listener->listening || listener->accept_count >= NET_MAX_QUEUE) {
                s->connected = 0;
                spin_unlock_irqrestore(&g_net_lock, irq);
                return listener->listening ? -EAGAIN : -ECONNREFUSED;
            }
            net_socket_t *child = (net_socket_t *)kmalloc(sizeof(net_socket_t));
            if (!child) {
                s->connected = 0;
                spin_unlock_irqrestore(&g_net_lock, irq);
                return -ENOMEM;
            }
            memset(child, 0, sizeof(*child));
            child->domain = AF_UNIX;
            child->type = SOCK_STREAM;
            child->protocol = s->protocol;
            child->bpf_prog_fd = -1;
            child->bound = 1;
            child->connected = 1;
            child->peer = s;
            s->peer = child;
            memcpy(child->local, listener->local, listener->local_len);
            child->local_len = listener->local_len;
            memcpy(child->peer_addr, s->local, s->local_len);
            child->peer_len = s->local_len;
            if (listener->accept_tail)
                listener->accept_tail->accept_next = child;
            else
                listener->accept_head = child;
            listener->accept_tail = child;
            listener->accept_count++;
            if (listener->waiter && listener->waiter->state == PROC_BLOCKED)
                proc_make_ready(listener->waiter);
        } else {
            s->peer = listener;
        }
        spin_unlock_irqrestore(&g_net_lock, irq);
        return 0;
    }
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
    } else if (s->type == SOCK_STREAM && (s->domain == AF_INET || s->domain == AF_INET6)) {
        net_socket_t *child = (net_socket_t *)kmalloc(sizeof(net_socket_t));
        if (!child) return -ENOMEM;
        memset(child, 0, sizeof(*child));

        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        net_socket_t *listener = find_bound_socket_locked(s->domain, SOCK_STREAM, connect_addr, peer_len);
        if (listener && listener->listening && listener->accept_count < NET_MAX_QUEUE) {
            child->domain = s->domain;
            child->type = SOCK_STREAM;
            child->protocol = s->protocol;
            child->bpf_prog_fd = -1;
            child->bound = 1;
            child->connected = 1;
            memcpy(child->local, connect_addr, peer_len);
            child->local_len = peer_len;
            memcpy(child->peer_addr, s->local, s->local_len);
            child->peer_len = s->local_len;
            child->peer = s;
            s->peer = child;
            s->connected = 1;
            s->local_tcp = 1;
            child->local_tcp = 1;
            if (listener->accept_tail)
                listener->accept_tail->accept_next = child;
            else
                listener->accept_head = child;
            listener->accept_tail = child;
            listener->accept_count++;
            if (listener->waiter && listener->waiter->state == PROC_BLOCKED)
                proc_make_ready(listener->waiter);
            spin_unlock_irqrestore(&g_net_lock, irq);
            net_tcp_drop_pcb(s);
            return 0;
        }
        spin_unlock_irqrestore(&g_net_lock, irq);
        kfree(child);

        if (s->domain != AF_INET || !s->tcp) {
            s->connected = 0;
            return -ECONNREFUSED;
        }

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
            if (task_has_unblocked_signal(cur)) {
                s->tcp_connecting = 0;
                s->connected = 0;
                return -EINTR;
            }
            uint64_t irq = spin_lock_irqsave(&g_net_lock);
            block_on_socket_locked(s, cur);
            spin_unlock_irqrestore(&g_net_lock, irq);
            a20_lwip_poll();
            sched();
            clear_socket_waiter(s, cur);
            if (task_has_unblocked_signal(cur)) {
                s->tcp_connecting = 0;
                s->connected = 0;
                return -EINTR;
            }
        }
        if (s->tcp_err != ERR_OK) {
            s->connected = 0;
            return -ECONNREFUSED;
        }
    }
    return 0;
}

int net_listen(int gfd, int backlog) {
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
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

int net_accept(int gfd, void *addr, size_t *addrlen, int flags) {
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) return -EINVAL;
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    if (s->domain == AF_ALG) {
        if (!s->bound) return -EINVAL;
        net_socket_t *child = (net_socket_t *)kmalloc(sizeof(net_socket_t));
        vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
        if (!child || !vf) {
            if (child) kfree(child);
            if (vf) kfree(vf);
            return -ENOMEM;
        }
        memset(child, 0, sizeof(*child));
        memset(vf, 0, sizeof(*vf));
        child->domain = AF_ALG;
        child->type = s->type;
        child->protocol = s->protocol;
        child->bpf_prog_fd = -1;
        child->nonblock = (flags & SOCK_NONBLOCK) != 0;
        child->bound = 1;
        child->connected = 1;
        memcpy(child->local, s->local, s->local_len);
        child->local_len = s->local_len;
        strncpy(child->alg_type, s->alg_type, sizeof(child->alg_type) - 1);
        strncpy(child->alg_name, s->alg_name, sizeof(child->alg_name) - 1);
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        int r = register_socket_locked(child);
        spin_unlock_irqrestore(&g_net_lock, irq);
        if (r < 0) {
            kfree(child);
            kfree(vf);
            return r;
        }
        vf->flags = O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
        vf->ref_count = 1;
        vf->ops = &g_net_ops;
        vf->priv = child;
        int newfd = vfs_alloc_fd(vf);
        if (newfd < 0) {
            net_vfile_close(vf);
            kfree(vf);
            return newfd;
        }
        if (addrlen) *addrlen = 0;
        return newfd;
    }
    if ((s->type != SOCK_STREAM && !(s->domain == AF_UNIX && s->type == SOCK_SEQPACKET)) ||
        (s->domain != AF_INET && s->domain != AF_INET6 && s->domain != AF_UNIX))
        return -EOPNOTSUPP;
    if (!s->listening)
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
        if (task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EINTR;
        }
        block_on_socket_locked(s, cur);
        spin_unlock_irqrestore(&g_net_lock, irq);
        a20_lwip_poll();
        sched();
        clear_socket_waiter(s, cur);
        if (task_has_unblocked_signal(cur))
            return -EINTR;
    }

    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) {
        if (child->tcp) tcp_abort(child->tcp);
        kfree(child);
        return -ENOMEM;
    }
    memset(vf, 0, sizeof(*vf));

    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    int r = register_socket_locked(child);
    spin_unlock_irqrestore(&g_net_lock, irq);
    if (r < 0) {
        if (child->tcp) tcp_abort(child->tcp);
        kfree(child);
        kfree(vf);
        return r;
    }
    if (child->tcp) tcp_backlog_accepted(child->tcp);

    if (addr && addrlen && *addrlen > 0) {
        size_t n = child->peer_len < *addrlen ? child->peer_len : *addrlen;
        memcpy(addr, child->peer_addr, n);
        *addrlen = n;
    }

    child->nonblock = (flags & SOCK_NONBLOCK) ? 1 : s->nonblock;
    vf->flags = O_RDWR | (child->nonblock ? O_NONBLOCK : 0);
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
    if (s->domain == AF_ALG)
        return alg_socket_send(s, buf, len);
    if (s->udp && s->domain == AF_INET) {
        if (!s->bound) {
            uint16_t port = alloc_ephemeral_port_locked();
            sockaddr_loopback(s, port);
            ip_addr_t any;
            ip_addr_set_zero_ip4(&any);
            udp_bind(s->udp, &any, net_ntohs(port));
        }
        uint64_t irq = spin_lock_irqsave(&g_net_lock);
        net_socket_t *local_dst = NULL;
        const void *dst_addr = addr;
        size_t dst_len = addrlen;
        if (!dst_addr && s->connected) {
            dst_addr = s->peer_addr;
            dst_len = s->peer_len;
        }
        if (s->peer)
            local_dst = s->peer;
        else if (dst_addr)
            local_dst = find_bound_socket_locked(AF_INET, SOCK_DGRAM, dst_addr, dst_len);
        if (local_dst) {
            int rr = enqueue_msg_locked(local_dst, buf, len, s->local, s->local_len);
            spin_unlock_irqrestore(&g_net_lock, irq);
            return rr;
        }
        spin_unlock_irqrestore(&g_net_lock, irq);
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
    uint8_t unix_addr[NET_SOCKADDR_MAX];
    if (s->domain == AF_UNIX && addr) {
        size_t ulen = addrlen;
        int ur = unix_sockaddr_prepare(addr, addrlen, unix_addr, &ulen, NULL, 0);
        if (ur < 0) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return ur;
        }
        dst_addr = unix_addr;
        dst_len = ulen;
    }
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
    if (s->domain == AF_ALG)
        return alg_socket_recv(s, buf, len);
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
        if (task_has_unblocked_signal(cur)) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return -EINTR;
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
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
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
    if (level == SOL_SOCKET)
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
    else if (level == SOL_SOCKET)
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

int net_poll_events(int gfd, short events) {
    net_socket_t *s = file_socket(gfd);
    if (!s) return -ENOTSOCK;
    short revents = 0;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (s->closed)
        revents |= POLLHUP;
    if ((events & POLLIN) &&
        (s->rx_head || s->closed ||
         (s->domain == AF_ALG && (strcmp(s->alg_type, "hash") == 0 || s->alg_last_len > 0))))
        revents |= POLLIN;
    if ((events & POLLOUT) && !s->closed)
        revents |= POLLOUT;
    spin_unlock_irqrestore(&g_net_lock, irq);
    return revents;
}
