/* Linux/A20OS socket frontend to the serialized F-Stack socket adapter. */
#include "executor.h"
#include "protocol.h"
#include "net/transport.h"
#include "net/socket/socket_internal.h"
#include "net/socket_options_state.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "sys/usercopy.h"
#include "net/stack.h"
#include "net/interface.h"

#define BSD_KIND_TCP 1u
#define BSD_KIND_UDP 2u
#define BSD_CONNECT_POLLS 4
#define BSD_RX_CHUNK 16384

typedef struct bsd_socket {
    net_socket_t *socket;
    struct bsd_socket *next;
    int fd;
    unsigned kind;
    int closing;
    int connecting;
    int connect_result;
    completion_t connect_done;
} bsd_socket_t;

static spinlock_t sockets_lock = SPINLOCK_INIT;
static bsd_socket_t *sockets;
static uint8_t *rx_stream, *rx_datagram;
static unsigned long rx_drops;

static bsd_socket_t *bsd(net_socket_t *s)
{
    return s ? (bsd_socket_t *)s->transport_private : NULL;
}

static int create_job(void *argument)
{
    net_socket_t *s = argument;
    return a20_bsd_port_socket_create(s->domain, s->type, s->protocol);
}

static void list_add(bsd_socket_t *state)
{
    uint64_t flags = spin_lock_irqsave(&sockets_lock);
    if (!__atomic_load_n(&state->closing, __ATOMIC_ACQUIRE)) {
        state->next = sockets;
        sockets = state;
    }
    spin_unlock_irqrestore(&sockets_lock, flags);
}

static void list_remove(bsd_socket_t *state)
{
    uint64_t flags = spin_lock_irqsave(&sockets_lock);
    bsd_socket_t **at = &sockets;
    while (*at) {
        if (*at == state) {
            *at = state->next;
            state->next = NULL;
            break;
        }
        at = &(*at)->next;
    }
    spin_unlock_irqrestore(&sockets_lock, flags);
}

static bsd_socket_t *state_attach(net_socket_t *s)
{
    bsd_socket_t *state = kcalloc(1, sizeof(*state));
    if (!state) return NULL;
    state->socket = s;
    state->fd = -1;
    completion_init(&state->connect_done);
    if (s->type == SOCK_STREAM) state->kind = BSD_KIND_TCP;
    else if (s->type == SOCK_DGRAM) state->kind = BSD_KIND_UDP;
    s->transport_private = state;
    return state;
}

static int socket_init(net_socket_t *s)
{
    if (!s) return -EINVAL;
    bsd_socket_t *state = state_attach(s);
    if (!state) return -ENOMEM;

    if ((s->domain != AF_INET && s->domain != AF_INET6) ||
        (s->type != SOCK_STREAM && s->type != SOCK_DGRAM)) return 0;
    int fd = a20_bsd_executor_call(create_job, s);
    if (fd < 0) return fd;
    state->fd = fd;
    list_add(state);
    return 0;
}

static int close_job(void *argument)
{
    return a20_bsd_port_socket_close((int)(intptr_t)argument);
}

static void socket_destroy(net_socket_t *s)
{
    bsd_socket_t *state = bsd(s);
    if (!state) return;
    __atomic_store_n(&state->closing, 1, __ATOMIC_RELEASE);
    list_remove(state);
    if (__atomic_load_n(&state->connecting, __ATOMIC_ACQUIRE)) {
        state->connect_result = -ECANCELED;
        __atomic_store_n(&state->connecting, 0, __ATOMIC_RELEASE);
        complete_all(&state->connect_done);
    }
    int fd = __atomic_exchange_n(&state->fd, -1, __ATOMIC_ACQ_REL);
    if (fd >= 0 && a20_bsd_executor_ready())
        (void)a20_bsd_executor_call(close_job, (void *)(intptr_t)fd);
}

static void state_release(net_socket_t *s)
{
    bsd_socket_t *state = bsd(s);
    if (!state) return;
    list_remove(state);
    s->transport_private = NULL;
    kfree(state);
}

typedef struct address_job {
    int fd;
    const void *address;
    size_t length;
} address_job_t;

static int bind_job(void *argument)
{
    address_job_t *job = argument;
    return a20_bsd_port_socket_bind(job->fd, job->address, job->length);
}

static int bind_pcb(net_socket_t *s, const void *address, size_t length)
{
    bsd_socket_t *state = bsd(s);
    if (!state || state->fd < 0) return -EOPNOTSUPP;
    address_job_t job = {state->fd, address, length};
    return a20_bsd_executor_call(bind_job, &job);
}

static int connect_job(void *argument)
{
    address_job_t *job = argument;
    return a20_bsd_port_socket_connect(job->fd, job->address, job->length);
}

static void connect_finish(bsd_socket_t *state, int result)
{
    net_socket_t *s = state->socket;
    uint64_t flags = net_sock_lock(s);
    s->tcp_connecting = 0;
    if (result == 0) {
        s->connected = 1;
        s->ever_connected = 1;
        s->peer_closed = 0;
    } else {
        s->pending_error = -result;
    }
    net_sock_unlock(s, flags);
    state->connect_result = result;
    __atomic_store_n(&state->connecting, 0, __ATOMIC_RELEASE);
    complete_all(&state->connect_done);
    wait_queue_wake_all(&s->read_waitq, 0, PROC_WAKE_EVENT);
    wait_queue_wake_all(&s->write_waitq, 0, PROC_WAKE_EVENT);
}

static int connect_transport(net_socket_t *s, const void *address, size_t length,
                             const void *peer, size_t peer_length)
{
    (void)peer; (void)peer_length;
    bsd_socket_t *state = bsd(s);
    if (!state || state->fd < 0) return -EOPNOTSUPP;
    if (__atomic_load_n(&state->connecting, __ATOMIC_ACQUIRE)) return -EALREADY;
    completion_init(&state->connect_done);
    state->connect_result = -EINPROGRESS;
    __atomic_store_n(&state->connecting, 1, __ATOMIC_RELEASE);
    uint64_t sf = net_sock_lock(s);
    s->tcp_connecting = 1;
    net_sock_unlock(s, sf);
    address_job_t job = {state->fd, address, length};
    int result = a20_bsd_executor_call(connect_job, &job);
    if (result != -EINPROGRESS) {
        __atomic_store_n(&state->connecting, 0, __ATOMIC_RELEASE);
        sf = net_sock_lock(s);
        s->tcp_connecting = 0;
        net_sock_unlock(s, sf);
        return result;
    }
    if (s->nonblock) return -EINPROGRESS;
    wait_for_completion(&state->connect_done);
    return state->connect_result;
}

typedef struct io_job {
    int fd, flags;
    const void *buffer;
    size_t length;
    const void *address;
    size_t address_length;
} io_job_t;

static int send_job(void *argument)
{
    io_job_t *job = argument;
    return a20_bsd_port_socket_send(job->fd, job->buffer, job->length,
                                    job->flags, job->address, job->address_length);
}

static int getsockname_job(void *argument)
{
    address_job_t *job = argument;
    size_t length = job->length;
    int result = a20_bsd_port_socket_getsockname(job->fd, (void *)job->address, &length);
    job->length = length;
    return result;
}

static void publish_local(net_socket_t *s, int fd)
{
    uint8_t address[NET_SOCKADDR_MAX];
    address_job_t job = {fd, address, sizeof(address)};
    int result = a20_bsd_executor_call(getsockname_job, &job);
    if (result < 0 || job.length > sizeof(address)) return;
    uint64_t flags = net_sock_lock(s);
    memcpy(s->local, address, job.length);
    s->local_len = job.length;
    s->bound = 1;
    net_sock_unlock(s, flags);
}

static int sendto_transport(net_socket_t *s, const void *buffer, size_t length,
                            int flags, const void *address, size_t address_length)
{
    bsd_socket_t *state = bsd(s);
    if (!state || state->fd < 0) return -EPIPE;
    io_job_t job = {state->fd, flags, buffer, length, address, address_length};
    uint64_t start = timer_get_ticks();
    for (;;) {
        int result = a20_bsd_executor_call(send_job, &job);
        if (result != -EAGAIN || s->nonblock || (flags & MSG_DONTWAIT)) {
            if (result >= 0 && !s->bound) publish_local(s, state->fd);
            return result;
        }
        if (net_task_has_unblocked_signal(proc_current())) return -ERESTARTSYS;
        if (net_socket_wait_expired(s, start, 1)) return -EAGAIN;
        a20_bsd_executor_notify();
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(1));
    }
}

static int shutdown_job(void *argument)
{
    io_job_t *job = argument;
    return a20_bsd_port_socket_shutdown(job->fd, (int)(intptr_t)job->buffer,
                                        (int)job->flags);
}

static int shutdown_transport(net_socket_t *s, int receive, int transmit)
{
    bsd_socket_t *state = bsd(s);
    if (!state || state->fd < 0) return s->domain == AF_UNIX ? 0 : -ENOTCONN;
    io_job_t job = {.fd = state->fd, .flags = transmit,
                    .buffer = (void *)(intptr_t)receive};
    return a20_bsd_executor_call(shutdown_job, &job);
}

typedef struct listen_job { int fd, backlog; } listen_job_t;
static int listen_job_run(void *argument)
{
    listen_job_t *job = argument;
    return a20_bsd_port_socket_listen(job->fd, job->backlog);
}

static int listen_transport(net_socket_t *s, int backlog)
{
    bsd_socket_t *state = bsd(s);
    if (!state || state->fd < 0 || s->type != SOCK_STREAM) return -EOPNOTSUPP;
    listen_job_t job = {state->fd, backlog};
    return a20_bsd_executor_call(listen_job_run, &job);
}

static void accept_pending(bsd_socket_t *state)
{
    net_socket_t *listener = state->socket;
    if (!listener->listening || state->kind != BSD_KIND_TCP) return;
    for (unsigned turn = 0; turn < BSD_CONNECT_POLLS; ++turn) {
        uint8_t peer[NET_SOCKADDR_MAX];
        size_t peer_len = sizeof(peer);
        int fd = a20_bsd_port_socket_accept(state->fd, peer, &peer_len);
        if (fd == -EAGAIN) return;
        if (fd < 0) {
            if (fd == -ECONNABORTED || fd == -EINTR) continue;
            return;
        }

        net_socket_t *child = net_socket_alloc();
        if (!child) {
            (void)a20_bsd_executor_call(close_job, (void *)(intptr_t)fd);
            return;
        }
        child->domain = listener->domain;
        child->type = SOCK_STREAM;
        child->protocol = listener->protocol;
        child->nonblock = 0;
        child->snd_buf = listener->snd_buf;
        child->rcv_buf = listener->rcv_buf;
        resource_namespace_t *net_ns = resource_ns_get(listener->net_ns);
        if (net_ns) {
            resource_ns_put(child->net_ns);
            child->net_ns = net_ns;
        }
        bsd_socket_t *child_state = state_attach(child);
        if (!child_state) {
            (void)a20_bsd_executor_call(close_job, (void *)(intptr_t)fd);
            net_socket_free(child);
            return;
        }
        child_state->fd = fd;
        list_add(child_state);

        uint8_t local[NET_SOCKADDR_MAX];
        size_t local_len = sizeof(local);
        if (a20_bsd_port_socket_getsockname(fd, local, &local_len) == 0) {
            memcpy(child->local, local, local_len);
            child->local_len = local_len;
            child->bound = 1;
        }
        if (peer_len <= sizeof(child->peer_addr)) {
            memcpy(child->peer_addr, peer, peer_len);
            child->peer_len = peer_len;
        }
        child->connected = 1;
        child->ever_connected = 1;

        int registered = net_register_socket_locked(child);
        if (registered < 0) {
            socket_destroy(child);
            net_socket_free(child);
            return;
        }
        uint64_t flags = net_sock_lock(listener);
        int pushed = listener->closed ? -ENOTCONN :
            net_accept_queue_push_locked(listener, child);
        net_sock_unlock(listener, flags);
        if (pushed < 0) {
            (void)net_socket_unregister(child);
            socket_destroy(child);
            net_socket_free(child);
            return;
        }
        wait_queue_wake_all(&listener->accept_waitq, 0, PROC_WAKE_EVENT);
    }
}

static unsigned transport_flags(const net_socket_t *s)
{
    bsd_socket_t *state = bsd((net_socket_t *)s);
    if (!state || state->fd < 0 || __atomic_load_n(&state->closing, __ATOMIC_ACQUIRE))
        return 0;
    return state->kind;
}

static int has_transport(const net_socket_t *s) { return transport_flags(s) != 0; }
static int has_tcp(const net_socket_t *s) { return !!(transport_flags(s) & BSD_KIND_TCP); }
static uint32_t buffer_default(int send) { (void)send; return 65536; }

static int option_validate(net_socket_t *s, int level, int option,
                           const void *value, size_t length)
{
    (void)s; (void)level; (void)option; (void)value; (void)length;
    return 0;
}

typedef struct option_job { int fd, level, option, value; } option_job_t;
static int option_job_run(void *argument)
{
    option_job_t *job = argument;
    return a20_bsd_port_socket_set_int(job->fd, job->level, job->option, job->value);
}

static void options_apply(net_socket_t *s, unsigned mask)
{
    bsd_socket_t *state = bsd(s);
    if (!state || state->fd < 0) return;
    option_job_t job = {.fd = state->fd};
    if (mask & (1u << 1)) {
        job.level = IPPROTO_TCP; job.option = TCP_NODELAY; job.value = s->tcp_nodelay;
        (void)a20_bsd_executor_call(option_job_run, &job);
    }
    if (mask & (1u << 2)) {
        job.level = SOL_SOCKET; job.option = SO_KEEPALIVE; job.value = s->keepalive;
        (void)a20_bsd_executor_call(option_job_run, &job);
    }
    if (mask & (1u << 3)) {
        job.level = SOL_SOCKET; job.option = SO_REUSEADDR; job.value = s->reuseaddr;
        (void)a20_bsd_executor_call(option_job_run, &job);
    }
    if (mask & (1u << 7)) {
        job.level = SOL_SOCKET; job.option = SO_SNDBUF; job.value = (int)s->snd_buf;
        (void)a20_bsd_executor_call(option_job_run, &job);
    }
    if (mask & (1u << 8)) {
        job.level = SOL_SOCKET; job.option = SO_RCVBUF; job.value = (int)s->rcv_buf;
        (void)a20_bsd_executor_call(option_job_run, &job);
    }
}

static uint32_t buffer_ceiling(net_socket_t *s, int is_snd)
{
    if (!s) return 0;
    if (s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET)
        return is_snd ? NET_MAX_PAYLOAD : NET_DGRAM_RCV_BUF_DEFAULT;
    return 65536;
}

static uint32_t buffer_in_force(net_socket_t *s, int is_snd)
{
    if (!s) return 0;
    uint32_t value = is_snd ? s->snd_buf : s->rcv_buf;
    uint32_t ceiling = buffer_ceiling(s, is_snd);
    return value < ceiling ? value : ceiling;
}

static void ip_effective(net_socket_t *s, uint8_t *ttl, uint8_t *tos,
                         uint8_t *mc_ttl)
{
    if (!s) return;
    if (ttl) *ttl = s->ip_ttl_set ? s->ip_ttl : 64;
    if (tos) *tos = s->ip_tos_set ? s->ip_tos : 0;
    if (mc_ttl) *mc_ttl = s->mc_ttl ? s->mc_ttl : 1;
}

static void ip_options_apply(net_socket_t *s)
{
    bsd_socket_t *state = bsd(s);
    if (!state || state->fd < 0 || s->domain != AF_INET) return;
    option_job_t job = {.fd = state->fd, .level = IPPROTO_IP};
    uint8_t ttl, tos, mc_ttl;
    ip_effective(s, &ttl, &tos, &mc_ttl);
    job.option = IP_TTL; job.value = ttl;
    (void)a20_bsd_executor_call(option_job_run, &job);
    job.option = IP_TOS; job.value = tos;
    (void)a20_bsd_executor_call(option_job_run, &job);
    job.option = 33; job.value = mc_ttl; /* Linux IP_MULTICAST_TTL wire value. */
    (void)a20_bsd_executor_call(option_job_run, &job);
    job.option = 34; job.value = s->mc_loop != 0;
    (void)a20_bsd_executor_call(option_job_run, &job);
}

static int bind_device(net_socket_t *s, const char *name)
{
    (void)s;
    return name && !name[0] ? 0 : -EOPNOTSUPP;
}

static int bound_device(net_socket_t *s, char name[16])
{
    (void)s;
    if (name) name[0] = 0;
    return 0;
}

static void receive_one(bsd_socket_t *state)
{
    net_socket_t *s = state->socket;
    for (unsigned i = 0; i < BSD_CONNECT_POLLS; ++i) {
        if (__atomic_load_n(&state->closing, __ATOMIC_ACQUIRE) || state->fd < 0 ||
            __atomic_load_n(&state->connecting, __ATOMIC_ACQUIRE)) return;
        uint64_t flags = net_sock_lock(s);
        int full = s->closed || s->rx_count >= NET_MAX_QUEUE;
        net_sock_unlock(s, flags);
        if (full) return;

        uint8_t address[NET_SOCKADDR_MAX];
        size_t address_length = sizeof(address);
        size_t capacity = state->kind == BSD_KIND_TCP ? BSD_RX_CHUNK : NET_MAX_PAYLOAD;
        void *buffer = state->kind == BSD_KIND_TCP ? (void *)rx_stream : (void *)rx_datagram;
        if (!buffer) return;
        int got = a20_bsd_port_socket_recv(state->fd, buffer, capacity, 0,
                                           address, &address_length);
        if (got == -EAGAIN) return;
        if (got < 0) {
            if (got == -ENOTCONN || got == -EINVAL) return;
            flags = net_sock_lock(s);
            s->pending_error = -got;
            s->peer_closed = 1;
            net_sock_unlock(s, flags);
            wait_queue_wake_all(&s->read_waitq, 0, PROC_WAKE_EVENT);
            return;
        }
        if (got == 0) {
            if (state->kind != BSD_KIND_TCP) return;
            flags = net_sock_lock(s);
            s->peer_closed = 1;
            net_sock_unlock(s, flags);
            wait_queue_wake_all(&s->read_waitq, 0, PROC_WAKE_EVENT);
            return;
        }
        flags = net_sock_lock(s);
        int queued = s->closed ? -ENOTCONN :
            net_enqueue_msg_locked(s, buffer, (size_t)got, address, address_length);
        net_sock_unlock(s, flags);
        if (queued < 0) {
            __atomic_add_fetch(&rx_drops, 1, __ATOMIC_RELAXED);
            return;
        }
        wait_queue_wake_all(&s->read_waitq, 0, PROC_WAKE_EVENT);
    }
}

void a20_bsd_socket_progress(void)
{
    uint64_t flags = spin_lock_irqsave(&sockets_lock);
    bsd_socket_t *pending = sockets;
    sockets = NULL;
    spin_unlock_irqrestore(&sockets_lock, flags);

    while (pending) {
        bsd_socket_t *state = pending;
        pending = state->next;
        state->next = NULL;
        if (!__atomic_load_n(&state->closing, __ATOMIC_ACQUIRE)) {
            /* Reinsert before processing so close can always find and remove
             * the state. The detached pending list makes this a finite pass. */
            list_add(state);
        }
        net_socket_t *s = net_socket_ref(state->socket);
        if (!s) continue;
        accept_pending(state);
        if (__atomic_load_n(&state->connecting, __ATOMIC_ACQUIRE)) {
            uint8_t address[NET_SOCKADDR_MAX];
            size_t length = sizeof(address);
            int peer = a20_bsd_port_socket_getpeername(state->fd, address, &length);
            int error = peer == 0 ? 0 : a20_bsd_port_socket_error(state->fd);
            if (error > 0) {
                connect_finish(state, -error);
            } else if (peer == 0) {
                connect_finish(state, 0);
            }
        }
        if (!__atomic_load_n(&state->connecting, __ATOMIC_ACQUIRE))
            receive_one(state);
        net_socket_free(s);
    }
}

int a20_bsd_socket_runtime_init(void)
{
    if (!rx_stream) rx_stream = kmalloc(BSD_RX_CHUNK);
    if (!rx_datagram) rx_datagram = kmalloc(NET_MAX_PAYLOAD);
    return rx_stream && rx_datagram ? 0 : -ENOMEM;
}

int a20_bsd_socket_format_status(char *buffer, size_t size)
{
    return snprintf(buffer, size, "bsd_rx_drops: %lu\n", rx_drops);
}

static void close_pcb(net_socket_t *s) { socket_destroy(s); }
static void drop_pcb(net_socket_t *s) { socket_destroy(s); }
static void accept_child_ready(net_socket_t *s) { (void)s; }
static void recved(net_socket_t *s, size_t length) { (void)s; (void)length; }
static int send_udp_control(net_socket_t *s, const void *b, size_t n, int f,
                            const void *a, size_t an, const net_datagram_control_t *c)
{ (void)s; (void)b; (void)n; (void)f; (void)a; (void)an; (void)c; return -EOPNOTSUPP; }
static int pmtu_discover(net_socket_t *s, int mode, int set)
{ (void)s; (void)mode; (void)set; return -EOPNOTSUPP; }
static int send_raw(net_socket_t *s, const void *b, size_t n, const void *a,
                    size_t an, int h, int t)
{ (void)s; (void)b; (void)n; (void)a; (void)an; (void)h; (void)t; return -EOPNOTSUPP; }
#define BSD_SIOCGIFNAME     0x8910
#define BSD_SIOCGIFCONF     0x8912
#define BSD_SIOCGIFINDEX    0x8933
#define BSD_SIOCGIFFLAGS    0x8913
#define BSD_SIOCSIFFLAGS    0x8914
#define BSD_SIOCGIFADDR     0x8915
#define BSD_SIOCSIFADDR     0x8916
#define BSD_SIOCGIFDSTADDR  0x8917
#define BSD_SIOCSIFDSTADDR  0x8918
#define BSD_SIOCGIFBRDADDR  0x8919
#define BSD_SIOCSIFBRDADDR  0x891a
#define BSD_SIOCGIFNETMASK  0x891b
#define BSD_SIOCSIFNETMASK  0x891c
#define BSD_SIOCGIFMTU      0x8921
#define BSD_SIOCSIFMTU      0x8922
#define BSD_SIOCGIFHWADDR   0x8927
#define BSD_SIOCGIFTXQLEN   0x8942
#define BSD_SIOCSIFTXQLEN   0x8943
#define BSD_IFNAMSIZ        16
#define BSD_IFF_UP          0x1
#define BSD_IFF_BROADCAST   0x2
#define BSD_IFF_LOOPBACK    0x8
#define BSD_IFF_RUNNING     0x40
#define BSD_ARPHRD_ETHER    1
#define BSD_ARPHRD_LOOPBACK 772

struct bsd_ifreq {
    char name[BSD_IFNAMSIZ];
    union {
        uint8_t raw[16];
        struct { uint16_t family, port; uint32_t address; uint8_t zero[8]; } addr;
        struct { unsigned long start, end; unsigned short base; uint8_t irq, dma; unsigned short port; } map;
        void *data;
        short flags;
        int value;
    } u;
};
struct bsd_ifconf { int length; void *buffer; };
_Static_assert(sizeof(struct bsd_ifreq) == 40, "Linux ifreq wire size");

static int ifreq_name_index(const char *name)
{
    if (!name[0] || !strcmp(name, "a20n0") || !strcmp(name, "en2") ||
        !strcmp(name, "eth0") || !strcmp(name, "net0"))
        return net_stack_if_default_index();
    if (!strcmp(name, "lo")) return 2;
    return -ENODEV;
}

static int ifreq_put_addr(void *uarg, uint16_t family, const void *bytes,
                          size_t length)
{
    uint8_t out[16];
    memset(out, 0, sizeof(out));
    out[0] = (uint8_t)(family & 0xff);
    out[1] = (uint8_t)(family >> 8);
    size_t offset = family == 2 ? 4 : 2;
    if (length > sizeof(out) - offset) length = sizeof(out) - offset;
    memcpy(out + offset, bytes, length);
    return copy_to_user((uint8_t *)uarg + BSD_IFNAMSIZ, out, sizeof(out)) < 0
           ? -EFAULT : 0;
}

static int ifconf_fill(void *uarg)
{
    struct bsd_ifconf conf;
    if (copy_from_user(&conf, uarg, sizeof(conf)) < 0) return -EFAULT;
    int needed = 2 * (int)sizeof(struct bsd_ifreq);
    if (!conf.buffer) {
        conf.length = needed;
        return copy_to_user(uarg, &conf, sizeof(conf)) < 0 ? -EFAULT : 0;
    }
    int used = 0;
    for (int index = 1; index <= 2; ++index) {
        if (used + (int)sizeof(struct bsd_ifreq) > conf.length) break;
        struct bsd_ifreq row;
        memset(&row, 0, sizeof(row));
        if (index == 1) strcpy(row.name, "a20n0"); else strcpy(row.name, "lo");
        uint8_t ip[4], mask[4], gateway[4];
        if (net_stack_if_get_addr((unsigned)index, ip, mask, gateway) == 0) {
            row.u.addr.family = 2;
            memcpy(&row.u.addr.address, ip, 4);
        }
        if (copy_to_user((uint8_t *)conf.buffer + used, &row, sizeof(row)) < 0)
            return -EFAULT;
        used += sizeof(row);
    }
    conf.length = used;
    return copy_to_user(uarg, &conf, sizeof(conf)) < 0 ? -EFAULT : 0;
}

static int interface_ioctl(net_socket_t *s, unsigned long request, void *argument)
{
    (void)s;
    if (!argument) return -EFAULT;
    if (request == BSD_SIOCGIFCONF) return ifconf_fill(argument);
    struct bsd_ifreq ifr;
    if (copy_from_user(&ifr, argument, sizeof(ifr)) < 0) return -EFAULT;
    ifr.name[BSD_IFNAMSIZ - 1] = 0;
    int index = request == BSD_SIOCGIFNAME
        ? ifr.u.value : ifreq_name_index(ifr.name);
    if (index < 0) return index;

    switch (request) {
    case BSD_SIOCGIFNAME:
        if (index == 1) strcpy(ifr.name, "a20n0");
        else if (index == 2) strcpy(ifr.name, "lo");
        else return -ENODEV;
        return copy_to_user(argument, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    case BSD_SIOCGIFINDEX:
    case BSD_SIOCGIFMTU:
    case BSD_SIOCGIFTXQLEN: {
        int value = request == BSD_SIOCGIFINDEX ? index :
                    request == BSD_SIOCGIFTXQLEN ? 1000 :
                    net_stack_if_mtu((unsigned)index);
        if (value < 0) return value;
        return copy_to_user((uint8_t *)argument + BSD_IFNAMSIZ, &value,
                            sizeof(value)) < 0 ? -EFAULT : 0;
    }
    case BSD_SIOCSIFMTU:
        if (ifr.u.value < 0 || ifr.u.value > 65535) return -EINVAL;
        return net_stack_if_set_mtu((unsigned)index, (uint16_t)ifr.u.value);
    case BSD_SIOCSIFTXQLEN:
        return 0;
    case BSD_SIOCGIFFLAGS: {
        short value = BSD_IFF_BROADCAST;
        if (index == 2) value |= BSD_IFF_LOOPBACK;
        if (net_stack_if_up((unsigned)index)) value |= BSD_IFF_UP | BSD_IFF_RUNNING;
        return copy_to_user((uint8_t *)argument + BSD_IFNAMSIZ, &value,
                            sizeof(value)) < 0 ? -EFAULT : 0;
    }
    case BSD_SIOCSIFFLAGS: {
        uint32_t value = (ifr.u.flags & BSD_IFF_UP) ? NLRT_IF_FLAGS_UP : 0;
        return net_stack_if_set_flags((unsigned)index, value,
                                      NLRT_IF_FLAGS_UP);
    }
    case BSD_SIOCGIFHWADDR: {
        uint8_t mac[6];
        if (index == 2) memset(mac, 0, sizeof(mac));
        else {
            int result = net_stack_if_hwaddr((unsigned)index, mac);
            if (result < 0) return result;
        }
        return ifreq_put_addr(argument,
                              index == 2 ? BSD_ARPHRD_LOOPBACK : BSD_ARPHRD_ETHER,
                              mac, sizeof(mac));
    }
    case BSD_SIOCGIFADDR:
    case BSD_SIOCGIFNETMASK:
    case BSD_SIOCGIFDSTADDR:
    case BSD_SIOCGIFBRDADDR: {
        uint8_t ip[4], mask[4], gateway[4];
        int result = net_stack_if_get_addr((unsigned)index, ip, mask, gateway);
        if (result < 0) return result;
        if (request == BSD_SIOCGIFADDR)
            return ifreq_put_addr(argument, 2, ip, 4);
        if (request == BSD_SIOCGIFNETMASK)
            return ifreq_put_addr(argument, 2, mask, 4);
        uint8_t broadcast[4];
        for (unsigned i = 0; i < 4; ++i) broadcast[i] = ip[i] | (uint8_t)~mask[i];
        return ifreq_put_addr(argument, 2, broadcast, 4);
    }
    case BSD_SIOCSIFADDR:
    case BSD_SIOCSIFNETMASK: {
        uint8_t ip[4], mask[4], gateway[4];
        int result = net_stack_if_get_addr((unsigned)index, ip, mask, gateway);
        if (result < 0) return result;
        if (ifr.u.addr.family != 2) return -EINVAL;
        if (request == BSD_SIOCSIFADDR)
            memcpy(ip, &ifr.u.addr.address, 4);
        else
            memcpy(mask, &ifr.u.addr.address, 4);
        return net_stack_if_set_addr((unsigned)index, ip, mask, gateway);
    }
    case BSD_SIOCSIFDSTADDR:
    case BSD_SIOCSIFBRDADDR:
        return -EOPNOTSUPP;
    default:
        return -EOPNOTSUPP;
    }
}

const net_socket_ops_t a20_bsd_socket_ops = {
    .open = socket_init,
    .close = socket_destroy,
    .release = state_release,
    .bind = bind_pcb,
    .connect = connect_transport,
    .listen = listen_transport,
    .send = sendto_transport,
    .shutdown = shutdown_transport,
    .accept_ready = accept_child_ready,
    .recved = recved,
    .close_pcb = close_pcb,
    .drop_pcb = drop_pcb,
    .has_transport = has_transport,
    .has_stream = has_tcp,
    .flags = transport_flags,
};

const net_socket_opt_ops_t a20_bsd_socket_opt_ops = {
    .apply = options_apply,
    .validate = option_validate,
    .buf_default = buffer_default,
    .buf_ceiling = buffer_ceiling,
    .buf_in_force = buffer_in_force,
    .ip_apply = ip_options_apply,
    .ip_effective = ip_effective,
};

const net_endpoint_ops_t a20_bsd_endpoint_ops = {
    .bind_device = bind_device,
    .bound_device = bound_device,
    .udp_control = send_udp_control,
    .pmtu = pmtu_discover,
    .raw_send = send_raw,
    .ioctl = interface_ioctl,
};
