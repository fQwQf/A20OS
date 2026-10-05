#include "net/socket.h"
#include "net/socket_internal.h"
#include "mm/slab.h"
#include "ipc/ipc.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/objcache.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/cpu.h"
#include "core/lock.h"
#include "core/timer.h"
#include "drivers/net/virtio_net.h"
#include "drivers/core/driver_core.h"
#include "net/lwip_stack.h"
#include "lwip/tcp.h"

static obj_cache_t g_net_socket_cache = OBJ_CACHE_INIT("net_socket", net_socket_t, 128);

net_socket_t *net_socket_alloc(void) {
    net_socket_t *s = (net_socket_t *)obj_cache_alloc_zero(&g_net_socket_cache);
    if (s) {
        s->ipv6_checksum_offset = -1;
        s->reg_idx = -1;
        /* SO_SNDBUF / SO_RCVBUF defaults.  Zero has to mean "never set" for
         * setsockopt to be able to tell, so the default is written here rather
         * than being inferred from zero later.  These are the stack's own
         * compile-time limits, which is the honest default: a caller that never
         * asks gets exactly the window lwIP would have given it anyway. */
        s->snd_buf = TCP_SND_BUF;
        s->rcv_buf = TCP_WND;
        /* The creator's reference.  A socket that also reaches the registry
         * carries a second one, taken by net_register_socket_locked() and
         * dropped by one more net_socket_free() after the slot is released. */
        __atomic_store_n(&s->refs, 1, __ATOMIC_RELAXED);
        /* Provisional only: the authoritative lane comes from the bound address
         * and port, which net_inet_bind_pcb() recomputes. */
        s->lane = net_lane_of_cpu(cpu_current_id());
        wait_queue_init(&s->accept_waitq);
        wait_queue_init(&s->read_waitq);
        wait_queue_init(&s->write_waitq);
    }
    return s;
}

/*
 * Drop one reference; the last one frees the object.
 *
 * This used to free unconditionally, which was safe only because the single
 * global g_net_lock kept every holder of a socket pointer inside one critical
 * section.  With the registry sharded, a lookup that finds a socket in one
 * bucket and then needs a second bucket cannot keep the first one held, so a
 * pointer has to outlive its bucket lock on its own -- see net_socket_ref() in
 * socket_internal.h.  This also closes a pre-existing hole: net_socket_from_file()
 * drops its vfile reference before returning, so a concurrent close() on another
 * CPU could free the socket before the caller got as far as taking a lock.
 */
void net_socket_free(net_socket_t *s) {
    if (!s)
        return;
    if (__atomic_fetch_sub(&s->refs, 1, __ATOMIC_ACQ_REL) != 1)
        return;
    if (s->ch_buf) {
        kfree(s->ch_buf);
        s->ch_buf = NULL;
    }
    obj_cache_free(&g_net_socket_cache, s);
}

static int sockaddr_family(const void *addr, size_t len) {
    if (!addr || len < sizeof(uint16_t))
        return -EINVAL;
    return *(const uint16_t *)addr;
}

int net_task_has_unblocked_signal(task_t *t) {
    return signal_task_has_unblocked(t);
}

int net_socket_wait_expired(net_socket_t *s, uint64_t start, int for_write) {
    if (!s)
        return 0;
    uint64_t timeout = for_write ? s->send_timeout_ticks : s->recv_timeout_ticks;
    return timeout && (int64_t)(timer_get_ticks() - (start + timeout)) >= 0;
}

/*
 * Table searches.
 *
 * Both take one bucket lock at a time and take a reference on the socket they
 * return, because the caller has to be able to drop the lock before it takes
 * whatever second bucket it needs.  The caller releases the reference with
 * net_socket_free() once it is done with the pointer.
 */
typedef struct {
    int            domain;
    int            type;
    const void    *addr;
    size_t         addrlen;
    uint16_t       port;
    net_socket_t  *want;        /* bind-conflict only: the socket being bound */
    net_socket_t  *found;
} net_find_arg_t;

static int net_find_port_bound(const net_socket_t *s, int domain, int type,
                               uint16_t port)
{
    if (!s->bound || s->domain != domain || s->type != type)
        return 0;
    uint16_t s_port = 0;
    return net_sockaddr_port(s->local, s->local_len, &s_port) == 0 &&
           s_port == port;
}

static bool net_find_bound_slot(net_socket_t *s, int idx, void *arg)
{
    net_find_arg_t *a = arg;
    (void)idx;
    if (s->domain == AF_UNIX) {
        if (!s->bound || s->domain != a->domain || s->type != a->type)
            return false;
        if ((a->type == SOCK_STREAM || a->type == SOCK_SEQPACKET) &&
            s->connected && !s->listening)
            return false;
        if (!(s->local_len == a->addrlen &&
              memcmp(s->local, a->addr, a->addrlen) == 0))
            return false;
    } else {
        int d = s->domain;
        if (d != a->domain &&
            !((a->domain == AF_INET && d == AF_INET6) ||
              (a->domain == AF_INET6 && d == AF_INET)))
            return false;
        if (!net_find_port_bound(s, d, a->type, a->port))
            return false;
    }
    a->found = net_socket_ref(s);
    return true;
}

net_socket_t *net_find_bound_socket(int domain, int type,
                                    const void *addr, size_t addrlen)
{
    net_find_arg_t a = {
        .domain = domain, .type = type, .addr = addr, .addrlen = addrlen,
        .port = 0, .found = NULL,
    };
    if (domain != AF_UNIX && net_sockaddr_port(addr, addrlen, &a.port) < 0)
        return NULL;
    net_table_scan_all(net_find_bound_slot, &a);
    return a.found;
}

static int net_bind_sockets_overlap(net_socket_t *a, net_socket_t *b)
{
    if (a->domain == b->domain)
        return 1;
    /* IPv4 vs IPv6 only conflicts when the IPv6 side is dual-stack.
     * A v6-only (IPV6_V6ONLY) listener shares the port with IPv4. */
    if (a->domain == AF_INET && b->domain == AF_INET6)
        return !b->ipv6_v6only;
    if (a->domain == AF_INET6 && b->domain == AF_INET)
        return !a->ipv6_v6only;
    return 0;
}

static int net_bind_reuse_allowed(net_socket_t *new_s, net_socket_t *old_s)
{
    if (!new_s || !old_s || new_s->type != old_s->type)
        return 0;
    if (new_s->type == SOCK_RAW)
        return new_s->protocol == old_s->protocol;
    if (new_s->type == SOCK_STREAM)
        return new_s->reuseport && old_s->reuseport;
    return (new_s->reuseaddr && old_s->reuseaddr) ||
           (new_s->reuseport && old_s->reuseport);
}

static bool net_find_conflict_slot(net_socket_t *s, int idx, void *arg)
{
    net_find_arg_t *a = arg;
    (void)idx;
    if (s == a->want || !s->bound || s->type != a->want->type)
        return false;
    if (!net_bind_sockets_overlap(a->want, s))
        return false;
    if (!net_find_port_bound(s, s->domain, a->type, a->port))
        return false;
    if (net_bind_reuse_allowed(a->want, s))
        return false;
    a->found = net_socket_ref(s);
    return true;
}

/*
 * The socket that would collide with `new_s` binding `addr`, or NULL.  The
 * returned socket carries a reference the caller releases with
 * net_socket_free().
 */
static net_socket_t *net_find_bind_conflict(net_socket_t *new_s,
                                            const void *addr, size_t addrlen)
{
    if (!new_s || (new_s->domain != AF_INET && new_s->domain != AF_INET6))
        return NULL;
    uint16_t port = 0;
    if (net_sockaddr_port(addr, addrlen, &port) < 0)
        return NULL;
    net_find_arg_t a = {
        .domain = new_s->domain, .type = new_s->type, .addr = addr,
        .addrlen = addrlen, .port = port, .want = new_s, .found = NULL,
    };
    net_table_scan_all(net_find_conflict_slot, &a);
    return a.found;
}

void net_init(void) {
    /* The socket-table shard locks are initialised by the registry init
     * below; g_net_lock itself is gone. */
    net_socket_registry_init();
    /* Bus enumeration and driver core own transport discovery.  Network init
     * only consumes DEV_CLASS_NET; it must never run a second arch scanner. */
    a20_lwip_init();
    printf("[NET] socket layer initialized\n");
}

typedef struct {
    int used;
    int bound;
    int queued;
} net_status_arg_t;

static bool net_status_slot(net_socket_t *s, int idx, void *arg)
{
    net_status_arg_t *a = arg;
    (void)idx;
    a->used++;
    if (s->bound)
        a->bound++;
    a->queued += s->rx_count;
    return false;
}

int net_format_status(char *buf, size_t bufsz) {
    int n = a20_lwip_format_status(buf, bufsz);
    if (!buf || bufsz == 0)
        return 0;
    if (n < 0)
        n = 0;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;

    /* One bucket at a time: taking all of them to build a status line is the
     * livelock dcache.c documents, and a status line is not worth it. */
    net_status_arg_t a = { 0, 0, 0 };
    net_table_scan_all(net_status_slot, &a);

    /*
     * `max` is the profile's slot ceiling, not a free-slot reading: it is the
     * constant that decides whether a workload can be admitted at all, and it
     * is what tells a user why socket() started returning EMFILE.  Published
     * here because the table has no other observable -- the bitmap words are
     * internal and per-bucket, and a user process cannot count the slots it is
     * not holding.  Appended rather than inserted so the existing
     * "open= bound= queued=" prefix stays byte-compatible with anything that
     * reads it.
     */
    int m = snprintf(buf + n, bufsz - (size_t)n,
                     "syscall-sockets: open=%d bound=%d queued=%d max=%d\n",
                     a.used, a.bound, a.queued, NET_MAX_SOCKETS);
    if (m > 0)
        n += m;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;
    return n;
}

int net_socket_create(int domain, int type, int protocol) {
    int base_type = type & 0xf;
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6 &&
        domain != AF_NETLINK && domain != AF_ALG && domain != AF_PACKET)
        return -EAFNOSUPPORT;
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM && base_type != SOCK_RAW &&
        base_type != SOCK_SEQPACKET)
        return -EPROTOTYPE;
    if (domain == AF_ALG && base_type != SOCK_SEQPACKET)
        return -EPROTOTYPE;
    if (domain == AF_PACKET &&
        base_type != SOCK_RAW && base_type != SOCK_DGRAM)
        return -EPROTOTYPE;
    if (base_type == SOCK_RAW && domain != AF_NETLINK && domain != AF_PACKET &&
        ((domain != AF_INET && domain != AF_INET6) || protocol < 0 || protocol > 255))
        return -EPROTONOSUPPORT;
    if (domain == AF_NETLINK &&
        (base_type != SOCK_RAW ||
         (protocol != NETLINK_SOCK_DIAG &&
          protocol != NETLINK_KOBJECT_UEVENT &&
          protocol != NETLINK_ROUTE &&
          protocol != NETLINK_GENERIC)))
        return -EPROTONOSUPPORT;

    if (domain == AF_INET || domain == AF_INET6) {
        if (base_type == SOCK_STREAM && protocol != 0 && protocol != IPPROTO_TCP)
            return -EPROTONOSUPPORT;
        if (base_type == SOCK_DGRAM && protocol != 0 && protocol != IPPROTO_UDP)
            return -EPROTONOSUPPORT;
        if (base_type == SOCK_RAW && protocol == IPPROTO_TCP)
            return -EPROTONOSUPPORT;
        if (base_type == SOCK_RAW) {
            task_t *cur = proc_current();
            if (!cur || !(cur->cred.cap_effective & (1ULL << CAP_NET_RAW)))
                return -EACCES;
        }
    }

    if (domain == AF_PACKET && base_type == SOCK_RAW) {
        task_t *cur = proc_current();
        if (!cur || !(cur->cred.cap_effective & (1ULL << CAP_NET_RAW)))
            return -EACCES;
    }

    net_socket_t *s = net_socket_alloc();
    if (!s) {
        return -ENOMEM;
    }
    s->domain = domain;
    s->type = base_type;
    s->protocol = protocol;
    s->nonblock = (type & SOCK_NONBLOCK) != 0;
    s->ipv6_checksum_offset = -1;

    int init_r = net_inet_socket_init(s);
    if (init_r < 0) {
        net_socket_free(s);
        return init_r;
    }

    /* Registration picks the socket's bucket and takes that bucket's lock
     * itself; no other socket's bucket may be held here, and `s` is still
     * private to this thread, so nothing was lost by not holding one. */
    int r = net_register_socket_locked(s);
    if (r < 0) {
        net_inet_socket_destroy(s);
        net_socket_free(s);
        return r;
    }

    /* Sockets are readable+writable per F_GETFL; the socket type must not
     * leak into the file status flags (SOCK_RAW==3 would read back as an
     * invalid access mode and SOCK_STREAM as write-only). */
    return net_socket_install_file(s, O_RDWR |
                                   ((type & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
}

int net_socketpair_create(int domain, int type, int protocol, int out_gfd[2]) {
    if (domain != AF_UNIX)
        return -EOPNOTSUPP;
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
    /* Two buckets, taken in ascending order.  They are usually in different
     * shards precisely because registration starts from the creating CPU's own
     * bucket, so the pair lock is the common case here, not a corner. */
    net_bucket_pair_t pair = net_bucket_lock2(net_socket_bucket(sa),
                                              net_socket_bucket(sb));
    sa->peer = sb;
    sb->peer = sa;
    sa->connected = 1;
    sa->ever_connected = 1;
    sb->connected = 1;
    sb->ever_connected = 1;
    /* SO_PEERCRED for socketpair: both ends belong to this task. */
    {
        task_t *cur = proc_current();
        if (cur) {
            int32_t pid = cur->pid;
            int32_t uid = cur->cred.uid;
            int32_t gid = cur->cred.gid;
            sa->peer_pid = pid; sa->peer_uid = uid; sa->peer_gid = gid;
            sb->peer_pid = pid; sb->peer_uid = uid; sb->peer_gid = gid;
        }
    }
    net_bucket_unlock2(pair);
    out_gfd[0] = a;
    out_gfd[1] = b;
    return 0;
}

int net_bind(int gfd, const void *addr, size_t addrlen) {
    return net_bind_sock(net_socket_from_file(gfd), addr, addrlen);
}

int net_bind_sock(net_socket_t *s, const void *addr, size_t addrlen) {
    if (!s) return -ENOTSOCK;
    int family = sockaddr_family(addr, addrlen);
    if (family < 0) return family;
    int bind_family = family;
    if (bind_family == AF_UNSPEC && (s->domain == AF_INET || s->domain == AF_INET6))
        bind_family = s->domain;
    if (bind_family != s->domain) return -EAFNOSUPPORT;
    if (addrlen > NET_SOCKADDR_MAX)
        return -EINVAL;
    if (s->domain == AF_INET && addrlen < sizeof(net_sockaddr_in_t))
        return -EINVAL;
    if (s->domain == AF_INET6 && addrlen < sizeof(net_sockaddr_in6_t))
        return -EINVAL;
    uint8_t bind_addr[NET_SOCKADDR_MAX];
    memcpy(bind_addr, addr, addrlen);
    if (family == AF_UNSPEC && (s->domain == AF_INET || s->domain == AF_INET6))
        *(uint16_t *)bind_addr = (uint16_t)s->domain;
    size_t bind_len = addrlen;
    if (s->domain == AF_PACKET)
        return net_packet_socket_bind(s, addr, addrlen);
    if (s->bound) {
        if (s->type != SOCK_RAW)
            return -EINVAL;
        memcpy(s->local, bind_addr, bind_len);
        s->local_len = bind_len;
        return 0;
    }
    if (s->domain == AF_ALG)
        return net_alg_socket_bind(s, addr, addrlen);
    if (s->domain == AF_NETLINK)
        return net_netlink_bind(s, addr, addrlen);
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

    /* Conflict detection is a whole-table scan, so it takes one bucket at a
     * time and comes back with a reference rather than a raw pointer.  The
     * publication of s->local then happens under s's own bucket alone -- no
     * second bucket is needed, because the conflict check never mutates. */
    net_socket_t *conflict = net_find_bind_conflict(s, bind_addr, bind_len);
    if (conflict) {
        net_socket_free(conflict);
        return -EADDRINUSE;
    }
    int b = net_socket_bucket(s);
    uint64_t flags = net_bucket_lock(b);
    if (!net_socket_is_live(s)) {
        net_bucket_unlock(b, flags);
        return -ENOTSOCK;
    }
    memcpy(s->local, bind_addr, bind_len);
    s->local_len = bind_len;
    s->bound = 1;
    /* Authoritative lane: derived from the address and port the socket is now
     * bound to, which is the pair an inbound packet reproduces.  Set inside the
     * same critical section that publishes s->local so no reader can see one
     * without the other. */
    s->lane = net_socket_lane_of_addr(bind_addr, bind_len, s->lane);
    net_bucket_unlock(b, flags);
    return net_inet_bind_pcb(s, bind_addr, addrlen);
}

int net_connect(int gfd, const void *addr, size_t addrlen) {
    return net_connect_sock(net_socket_from_file(gfd), addr, addrlen);
}

int net_connect_sock(net_socket_t *s, const void *addr, size_t addrlen) {
    if (!s) return -ENOTSOCK;
    if (!addr || addrlen > NET_SOCKADDR_MAX) return -EINVAL;
    if (s->connected) return -EISCONN;
    if (s->domain == AF_UNIX)
        return net_unix_socket_connect(s, addr, addrlen);

    int family = sockaddr_family(addr, addrlen);
    if (family != s->domain) {
        if (!(s->domain == AF_INET6 && family == AF_INET))
            return -EAFNOSUPPORT;
    }

    int b = net_socket_bucket(s);
    uint64_t flags = net_bucket_lock(b);
    if (!net_socket_is_live(s)) {
        net_bucket_unlock(b, flags);
        return -ENOTSOCK;
    }
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    memcpy(s->peer_addr, addr, addrlen);
    s->peer_len = addrlen;
    net_bucket_unlock(b, flags);
    int r = net_inet_connect(s, addr, addrlen, addr, addrlen);
    if (r < 0 && r != -EINPROGRESS) {
        return r;
    }
    if (r == 0 && !s->connected) {
        s->connected = 1;
        s->ever_connected = 1;
    }
    return r;
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
    int b = net_socket_bucket(s);
    uint64_t irq = net_bucket_lock(b);
    if (!net_socket_is_live(s)) {
        net_bucket_unlock(b, irq);
        return -ENOTSOCK;
    }
    if (!s->bound) {
        net_sockaddr_in6_t local;
        memset(&local, 0, sizeof(local));
        local.sin6_family = AF_INET6;
        local.sin6_addr[15] = 1;
        memcpy(s->local, &local, sizeof(local));
        s->local_len = sizeof(local);
        s->bound = 1;
    }
    /* The delivery loop below walks buckets of its own, and it must not hold
     * s's bucket while it does: two buckets may only ever be taken in ascending
     * order, and the destinations can land on either side of s's.  So the
     * sender's address is copied out here and read back from the stack copy
     * down there. */
    uint8_t src_local[NET_SOCKADDR_MAX];
    size_t src_local_len = s->local_len;
    memcpy(src_local, s->local, src_local_len);
    net_bucket_unlock(b, irq);

    /* One destination bucket at a time.  The wake batch is flushed when it
     * fills -- exactly as the old single-lock loop did when it filled -- so
     * every destination is still visited and no wake is dropped. */
    for (int bucket = 0; bucket < NET_SOCK_BUCKETS; bucket++) {
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t bf = net_bucket_lock(bucket);
        int base = bucket << NET_SOCK_BUCKET_SHIFT;
        for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
            net_socket_t *dst = g_sockets[base + k];
            if (!dst || dst->closed || dst->domain != AF_INET6 ||
                dst->type != SOCK_RAW || dst->protocol != s->protocol)
                continue;
            if (!raw_ipv6_filter_passes(dst, buf, len))
                continue;
            if (net_enqueue_msg_locked(dst, buf, len, src_local,
                                       src_local_len) >= 0) {
                delivered++;
                (void)wait_queue_collect_one(
                    &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
                if (wake_q.count == PROC_WAKE_Q_CAPACITY)
                    break;
            }
        }
        net_bucket_unlock(bucket, bf);
        (void)proc_wake_q_flush(&wake_q);
    }
    return delivered ? (int)len : -ECONNREFUSED;
}

/* This stack has no out-of-band data path, so MSG_OOB cannot be honoured.
 * Rejecting it is the honest answer; ignoring the flag would quietly hand
 * back ordinary in-band bytes as if they were the urgent one.
 *
 * MSG_NOSIGNAL needs no case: the socket data plane has no SIGPIPE source at
 * all (the only generator is kernel/fs/pipe.c, for real pipes), so a write to a
 * dead peer already reports EPIPE without raising a signal, flag or no flag.
 * MSG_PEEK, MSG_TRUNC and MSG_WAITALL are honoured in
 * net_recvfrom_socket_meta(). */
static int net_msg_flags_check(int flags)
{
    return (flags & MSG_OOB) ? -EOPNOTSUPP : 0;
}

int net_sendto(int gfd, const void *buf, size_t len, int flags,
               const void *addr, size_t addrlen) {
    return net_sendto_sock(net_socket_from_file(gfd), buf, len, flags,
                           addr, addrlen);
}

int net_sendto_sock(net_socket_t *s, const void *buf, size_t len, int flags,
                    const void *addr, size_t addrlen) {
    if (!s) return -ENOTSOCK;
    int bad = net_msg_flags_check(flags);
    if (bad) return bad;
    if (len > NET_MAX_PAYLOAD && s->type != SOCK_STREAM) return -EMSGSIZE;
    int dontwait = s->nonblock || ((flags & MSG_DONTWAIT) != 0);
    if (s->domain == AF_ALG)
        return net_alg_socket_send(s, buf, len);
    if (s->domain == AF_PACKET)
        return net_packet_socket_send(s, buf, len, addr, addrlen);
    if (s->domain == AF_NETLINK) {
        if (s->protocol == NETLINK_SOCK_DIAG)
            return net_netlink_diag_request(s, buf, len, addr, addrlen);
        if (s->protocol == NETLINK_ROUTE)
            return net_netlink_route_request(s, buf, len, addr, addrlen);
        if (s->protocol == NETLINK_KOBJECT_UEVENT ||
            s->protocol == NETLINK_GENERIC)
            return net_netlink_uevent_send(s, buf, len, addr, addrlen);
        return -EPROTONOSUPPORT;
    }
    if (s->domain == AF_INET6 && s->type == SOCK_RAW)
        return net_sendto_raw_ipv6(s, (void *)buf, len, addr, addrlen);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        (s->udp || s->raw || s->tcp))
        return net_inet_sendto(s, buf, len, flags, addr, addrlen);
    /*
     * A stream socket with no pcb, in a family the stack owns, is a connection
     * lwIP tore down rather than a socket that never had one: lwip_tcp_err_cb()
     * NULLs s->tcp on RST and on every fatal error, so "s->tcp == NULL" is the
     * normal state of a dead TCP connection.  Dispatching on the presence of a
     * pcb therefore sent those writes past net_inet_sendto() into the generic
     * two-socket path below, which reported ENOTSOCK -- a claim that the fd is
     * not a socket at all, on an fd that is very much one.  send(2) on a
     * connection the peer reset owes the caller EPIPE, per the same Linux-ABI
     * rule net_inet_send_tcp() applies on the s->closed path.
     *
     * local_tcp is what keeps this off the fast path: a fast-mode socket drops
     * its pcb by design and is served by the shortcut below, so the exemption
     * has to be explicit rather than implied.
     */
    if (s->type == SOCK_STREAM && !s->local_tcp && !s->tcp &&
        (s->domain == AF_INET || s->domain == AF_INET6))
        return s->ever_connected ? -EPIPE : -ENOTCONN;
    if (s->domain == AF_UNIX)
        return net_unix_socket_sendto(s, buf, len, addr, addrlen);

    /*
     * Two sockets, two buckets, taken in ascending order.  The destination is
     * resolved first, outside any lock, because it comes from a whole-table
     * scan that has to walk the shards one at a time; the scan returns a
     * reference, which is what lets the bucket be dropped before the pair is
     * taken and is why this path cannot nest a third lock.
     */
    net_socket_t *dst = NULL;
    const void *dst_addr = addr;
    size_t dst_len = addrlen;
    uint8_t src_local[NET_SOCKADDR_MAX];
    size_t src_local_len = 0;
    int sb = net_socket_bucket(s);
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
        if (!dst_addr && s->connected) {
            dst_addr = s->peer_addr;
            dst_len = s->peer_len;
        }
        if (s->peer &&
            (s->type == SOCK_STREAM || s->type == SOCK_SEQPACKET ||
             net_socket_is_live(s->peer))) {
            dst = net_socket_ref(s->peer);
        }
        net_bucket_unlock(sb, sf);
    }
    if (!dst && s->peer) {
        /* The peer is gone (or was never a live socket); drop the stale
         * back-pointer and fall back to an address lookup. */
        uint64_t sf = net_bucket_lock(sb);
        if (s->peer) s->peer = NULL;
        net_bucket_unlock(sb, sf);
    }
    if (!dst && dst_addr)
        dst = net_find_bound_socket(s->domain, s->type, dst_addr, dst_len);
    if (!dst) {
        /* Linux ABI: writes on a once-connected stream whose endpoint vanished
         * report EPIPE. */
        if (s->type == SOCK_STREAM && s->ever_connected)
            return -EPIPE;
        return dst_addr ? -ECONNREFUSED : -EDESTADDRREQ;
    }

    int r;
    net_bucket_pair_t pair = net_bucket_lock2(sb, net_socket_bucket(dst));
    /* The search above ran without a lock, so the destination may have been
     * closed and unregistered in between.  A live socket's bucket never moves,
     * so re-checking in_registry under the pair is enough to know this is still
     * the socket the scan found. */
    if (!net_socket_is_live(dst)) {
        net_bucket_unlock2(pair);
        net_socket_free(dst);
        return (s->type == SOCK_STREAM && s->ever_connected)
                   ? -EPIPE
                   : (dst_addr ? -ECONNREFUSED : -EDESTADDRREQ);
    }
    if (len <= NET_MAX_PAYLOAD) {
        if (dst->rx_count >= NET_MAX_QUEUE && !dontwait) {
            net_bucket_unlock2(pair);
            int br = net_enqueue_msg_blocking(s, dst, buf, len, src_local,
                                              src_local_len, dontwait,
                                              s->send_timeout_ticks);
            net_socket_free(dst);
            return br;
        }
        r = net_enqueue_msg_locked(dst, buf, len, src_local, src_local_len);
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
    net_bucket_unlock2(pair);

    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > NET_MAX_PAYLOAD)
            chunk = NET_MAX_PAYLOAD;
        int br = net_enqueue_msg_blocking(s, dst, (const uint8_t *)buf + total,
                                          chunk, src_local, src_local_len,
                                          dontwait, s->send_timeout_ticks);
        if (br < 0) {
            net_socket_free(dst);
            return total ? (int)total : br;
        }
        total += (size_t)br;
    }
    net_socket_free(dst);
    return (int)total;
}

int net_recvfrom_socket_meta(net_socket_t *s, void *buf, size_t len, int flags,
                             void *addr, size_t *addrlen,
                             net_recv_meta_t *meta) {
    if (!s)
        return -ENOTSOCK;
    int bad = net_msg_flags_check(flags);
    if (bad)
        return bad;
    if (s->domain == AF_ALG)
        return net_alg_socket_recv(s, buf, len);
    int dontwait = (flags & MSG_DONTWAIT) != 0;
    uint64_t start = timer_get_ticks();
    if ((flags & MSG_WAITALL) && s->type == SOCK_STREAM && len > 0) {
        /* MSG_WAITALL: keep reading until len bytes have arrived, or until the
         * read is cut short by EOF, the receive timeout, or a signal.  A single
         * pass below already coalesces everything queued up to len, so this loop
         * only iterates when the queue runs dry mid-buffer, which is the one
         * case the flag adds.  WAITALL is stripped from the inner call so the
         * recursion stays one level deep, and the caller's SO_RCVTIMEO is
         * honoured against this outer start rather than restarting per read. */
        int inner = flags & ~MSG_WAITALL;
        size_t got = 0;
        while (got < len) {
            if (net_socket_wait_expired(s, start, 0))
                return (int)got;
            int n = net_recvfrom_socket_meta(s, (char *)buf + got, len - got,
                                             inner, got ? NULL : addr,
                                             got ? NULL : addrlen, meta);
            if (n < 0)
                return got ? (int)got : n;
            if (n == 0)
                return (int)got;
            got += (size_t)n;
        }
        return (int)got;
    }
    if (s->ch_ep) {
        /* Channel-backed path: plain data lives on the internal channel,
         * SCM_RIGHTS messages on the legacy queue.  Drain legacy first so
         * fd-carrying messages keep their delivery order. */
        for (;;) {
            a20_lwip_poll_waiter();
            int r = -EAGAIN;
            int rb = net_socket_bucket(s);
        uint64_t irq = net_bucket_lock(rb);
            if (s->rx_head) {
                if (flags & MSG_PEEK) {
                    net_msg_t *head = s->rx_head;
                    size_t avail = head->len - head->off;
                    size_t n = avail < len ? avail : len;
                    if (n)
                        memcpy(buf, net_msg_payload(head) + head->off, n);
                    r = (int)n;
                } else {
                    r = net_dequeue_msg_locked_meta(s, buf, len, addr,
                                                    addrlen, meta);
                }
                /* Stream coalescing: drain further legacy messages in the
                 * same recv (keeps SCM_RIGHTS fd coalescing semantics). */
                if (r >= 0 && s->type == SOCK_STREAM &&
                    !(flags & MSG_PEEK)) {
                    size_t total = (size_t)r;
                    while (total < len && s->rx_head) {
                        int nr = net_dequeue_msg_locked_meta(
                            s, (char *)buf + total, len - total, NULL, NULL,
                            meta);
                        if (nr <= 0)
                            break;
                        total += (size_t)nr;
                    }
                    r = (int)total;
                }
            }
            if (r == -EAGAIN && s->ch_ep)
                r = (flags & MSG_PEEK) ? unix_ch_peek(s, buf, len) :
                                         unix_ch_recv(s, buf, len);
            if (r >= 0) {
                /* Channel-backed delivery has no net_msg metadata; supply
                 * SCM_CREDENTIALS from the latest sender record captured in
                 * unix_ch_send when the receiver asked for it. */
                if (meta && s->passcred && !meta->has_cred && s->ch_ep) {
                    meta->has_cred = 1;
                    meta->cred_pid = s->ch_cred_pid;
                    meta->cred_uid = s->ch_cred_uid;
                    meta->cred_gid = s->ch_cred_gid;
                }
                proc_wake_q_t wq;
                proc_wake_q_init(&wq);
                (void)wait_queue_collect_one(&s->write_waitq, 0,
                                             PROC_WAKE_EVENT, &wq);
                net_bucket_unlock(rb, irq);
                (void)proc_wake_q_flush(&wq);
                return r;
            }
            if (r != -EAGAIN) {
                net_bucket_unlock(rb, irq);
                return r;
            }
            task_t *cur = proc_current();
            if (!cur) {
                net_bucket_unlock(rb, irq);
                return -EAGAIN;
            }
            if (net_task_has_unblocked_signal(cur)) {
                net_bucket_unlock(rb, irq);
                return -ERESTARTSYS;
            }
            if (net_socket_wait_expired(s, start, 0)) {
                net_bucket_unlock(rb, irq);
                return -EAGAIN;
            }
            if (s->nonblock || dontwait || s->closed || s->peer_closed ||
                s->shut_rd) {
                net_bucket_unlock(rb, irq);
                return (s->closed || s->peer_closed || s->shut_rd) ? 0 : -EAGAIN;
            }
            uint64_t deadline = s->recv_timeout_ticks ?
                                start + s->recv_timeout_ticks : 0;
            net_bucket_unlock(rb, irq);
            proc_wait_token_t token =
                proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
            if (!token.task)
                return -EAGAIN;
            wait_queue_entry_t entry = {0};
            irq = net_bucket_lock(rb);
            /* Re-check before linking: a send between the empty check and
             * the park would otherwise wake nobody and we would sleep
             * forever (classic lost-wakeup; the legacy loop does the same). */
            if (s->rx_head || a20_channel_readable(s->ch_ep)) {
                net_bucket_unlock(rb, irq);
                (void)proc_park_cancel(token);
                proc_park_finish(token);
                continue;
            }
            bool linked = wait_queue_link(&s->read_waitq, &entry, token, 0);
            net_bucket_unlock(rb, irq);
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
                return -EINTR;
        }
    }
    for (;;) {
        a20_lwip_poll_waiter();
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        int rb = net_socket_bucket(s);
        uint64_t irq = net_bucket_lock(rb);
        int rx_count_before = s->rx_count;
        net_msg_t *head = s->rx_head;
        size_t datagram_len =
            (head && s->type != SOCK_STREAM) ? head->len - head->off : 0;
        int r;
        if ((flags & MSG_PEEK) && head) {
            size_t avail = head->len - head->off;
            size_t n = avail < len ? avail : len;
            if (n)
                memcpy(buf, net_msg_payload(head) + head->off, n);
            if (addr && addrlen && *addrlen > 0) {
                size_t alen = head->addrlen < *addrlen
                                  ? head->addrlen : *addrlen;
                memcpy(addr, head->addr, alen);
                *addrlen = alen;
            }
            if (meta) {
                meta->has_pktinfo = head->has_pktinfo;
                meta->has_hoplimit = head->has_hoplimit;
                meta->has_tclass = head->has_tclass;
                meta->pktinfo_ifindex = head->pktinfo_ifindex;
                memcpy(meta->pktinfo_addr, head->pktinfo_addr,
                       sizeof(meta->pktinfo_addr));
                meta->hoplimit = head->hoplimit;
                meta->tclass = head->tclass;
            }
            r = ((flags & MSG_TRUNC) && s->type != SOCK_STREAM)
                    ? (int)avail : (int)n;
        } else {
            r = net_dequeue_msg_locked_meta(s, buf, len, addr, addrlen, meta);
            if (r >= 0 && (flags & MSG_TRUNC) &&
                s->type != SOCK_STREAM && datagram_len > (size_t)r)
                r = (int)datagram_len;
        }
        /* Stream coalescing: drain further queued messages into the same
         * buffer.  A peek must not consume anything, so it stops at the head
         * even when more is queued -- a peek is a read without a dequeue. */
        if (r > 0 && s->type == SOCK_STREAM && !(flags & MSG_PEEK)) {
            size_t total = (size_t)r;
            while (total < len && s->rx_head) {
                int nr = net_dequeue_msg_locked_meta(s, (char *)buf + total, len - total, NULL, NULL, meta);
                if (nr <= 0)
                    break;
                total += (size_t)nr;
            }
            r = (int)total;
        }
        if (r != -EAGAIN || s->nonblock || dontwait || s->closed || s->peer_closed || s->shut_rd) {
            if (r == -EAGAIN && (s->closed || s->peer_closed || s->shut_rd))
                r = 0;
            /* Accounting is deliberately outside the bucket lock: net_tcp_recved()
             * takes g_lwip_lock, and the two must never nest.  A peeked stream
             * message is charged too -- the bytes are in the receive queue
             * either way, and never charging them would stall the window once
             * the queue filled.  The cost is that repeated peeks of the same
             * head keep re-charging it. */
            int recved = (r > 0 && s->type == SOCK_STREAM) ? r : 0;
            if (r > 0 && s->rx_count < rx_count_before)
                (void)wait_queue_collect_one(
                    &s->write_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            if (r > 0 && s->rx_head)
                (void)wait_queue_collect_one(
                    &s->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
            net_bucket_unlock(rb, irq);
            (void)proc_wake_q_flush(&wake_q);
            if (recved)
                net_tcp_recved(s, (size_t)recved);
            return r;
        }
        task_t *cur = proc_current();
        if (!cur) {
            net_bucket_unlock(rb, irq);
            return -EAGAIN;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_bucket_unlock(rb, irq);
            return -ERESTARTSYS;
        }
        if (net_socket_wait_expired(s, start, 0)) {
            net_bucket_unlock(rb, irq);
            return -EAGAIN;
        }
        uint64_t deadline = s->recv_timeout_ticks ?
                            start + s->recv_timeout_ticks : 0;
        net_bucket_unlock(rb, irq);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        irq = net_bucket_lock(rb);
        if (s->rx_head || s->nonblock || dontwait || s->closed ||
            s->peer_closed || s->shut_rd) {
            net_bucket_unlock(rb, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        if (net_task_has_unblocked_signal(cur)) {
            net_bucket_unlock(rb, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ERESTARTSYS;
        }
        bool linked = wait_queue_link(&s->read_waitq, &entry, token, 0);
        net_bucket_unlock(rb, irq);
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

int net_recvfrom_meta(int gfd, void *buf, size_t len, int flags,
                      void *addr, size_t *addrlen, net_recv_meta_t *meta) {
    return net_recvfrom_socket_meta(net_socket_from_file(gfd), buf, len,
                                    flags, addr, addrlen, meta);
}

int net_recvfrom(int gfd, void *buf, size_t len, int flags,
                 void *addr, size_t *addrlen) {
    return net_recvfrom_meta(gfd, buf, len, flags, addr, addrlen, NULL);
}
