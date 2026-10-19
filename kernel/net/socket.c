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
#include "core/panic.h"
#include "core/timer.h"
#include "drivers/net/virtio_net.h"
#include "drivers/core/driver_core.h"
#include "net/lwip_stack.h"
#include "lwip/tcp.h"

static obj_cache_t g_net_socket_cache = OBJ_CACHE_INIT("net_socket", net_socket_t, 128);

/*
 * Reference-count ledger; see the block above net_socket_ref() in
 * socket_internal.h for what each number means and why a leak and a double free
 * need different counters to be visible.
 *
 * The ledger itself now lives with the rest of the reference lifecycle in
 * kernel/net/socket_registry.c -- Rust-owned under NET_SOCKET_REGISTRY_IMPL=rust
 * -- because the counters only mean anything next to the transition that moves
 * them.  What is left here is the socket object itself.
 */

/*
 * -ENOTCONN attribution; see the enum above in socket_internal.h.  The four
 * reasons in the first group are the impl-notes-net.md §8.5 window and the rest
 * are ordinary "no connection" errors, and keeping the two apart in one number
 * is the entire reason this table exists.
 */
static const char *const g_net_notconn_why[NET_NOTCONN__COUNT] = {
    [NET_NOTCONN_SEND_TCP_NO_PEER]        = "send_tcp: peer back-pointer absent",
    [NET_NOTCONN_SEND_TCP_PEER_GONE]      = "send_tcp: peer died before the ordered pair",
    [NET_NOTCONN_BLOCKING_PRE_PARK_RACE]  = "enqueue_blocking: race before park",
    [NET_NOTCONN_BLOCKING_POST_PARK_RACE] = "enqueue_blocking: race after park",
    [NET_NOTCONN_SENDTO_NOT_CONNECTED]    = "sendto: stream socket never connected",
    [NET_NOTCONN_SEND_TCP_NOT_CONNECTED]  = "send_tcp: not connected",
    [NET_NOTCONN_SEND_TCP_SHUT_WR]        = "send_tcp: write side shut down",
    [NET_NOTCONN_GETPEERNAME]             = "getpeername: not connected",
    [NET_NOTCONN_PEERPIDFD]               = "SO_PEERPIDFD: no peer pid",
    [NET_NOTCONN_VFS_WRITE_NO_PCB]        = "vfs write: inet stream has no pcb",
    [NET_NOTCONN_ENQUEUE_META]            = "enqueue_meta: destination gone",
    [NET_NOTCONN_ENQUEUE_PBUF]            = "enqueue_pbuf: destination gone",
    [NET_NOTCONN_BLOCKING_PEER_MISMATCH]  = "enqueue_blocking: destination is not the peer",
};

static volatile int g_net_notconn_n[NET_NOTCONN__COUNT];
static volatile int g_net_notconn_window;

int net_notconn_window_total(void)
{
    return __atomic_load_n(&g_net_notconn_window, __ATOMIC_RELAXED);
}

int net_notconn(net_notconn_reason_t why)
{
    if ((unsigned)why >= NET_NOTCONN__COUNT)
        why = NET_NOTCONN_SEND_TCP_NOT_CONNECTED;
    __atomic_fetch_add(&g_net_notconn_n[why], 1, __ATOMIC_RELAXED);
    switch (why) {
    case NET_NOTCONN_SEND_TCP_NO_PEER:
    case NET_NOTCONN_SEND_TCP_PEER_GONE:
    case NET_NOTCONN_BLOCKING_PRE_PARK_RACE:
    case NET_NOTCONN_BLOCKING_POST_PARK_RACE:
        __atomic_fetch_add(&g_net_notconn_window, 1, __ATOMIC_RELAXED);
        break;
    default:
        break;
    }
    return -ENOTCONN;
}

int net_notconn_format(char *buf, size_t bufsz)
{
    int off = 0;
    int total = 0;
    for (int i = 0; i < NET_NOTCONN__COUNT; i++)
        total += __atomic_load_n(&g_net_notconn_n[i], __ATOMIC_RELAXED);
    int n = snprintf(buf, bufsz,
                     "\nnet_notconn: total=%d window=%d reasons=%d\n", total,
                     net_notconn_window_total(), NET_NOTCONN__COUNT);
    if (n < 0)
        return 0;
    off = n;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;
    for (int i = 0; i < NET_NOTCONN__COUNT; i++) {
        int c = __atomic_load_n(&g_net_notconn_n[i], __ATOMIC_RELAXED);
        if (!c)
            continue;
        n = snprintf(buf + off, bufsz - (size_t)off, "net_notconn_%d: %s %d\n",
                     i, g_net_notconn_why[i] ? g_net_notconn_why[i] : "unnamed",
                     c);
        if (n < 0)
            break;
        off += n;
        if ((size_t)off >= bufsz)
            return (int)bufsz - 1;
    }
    return off;
}

net_socket_t *net_socket_alloc(void) {
    net_socket_t *s = (net_socket_t *)obj_cache_alloc_zero(&g_net_socket_cache);
    if (s) {
        s->net_ns = resource_ns_task_get(proc_current(), PROCNS_NET);
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
        net_sock_ref_init(s);
        /* Provisional only: the authoritative lane comes from the bound address
         * and port, which net_inet_bind_pcb() recomputes. */
        net_socket_lane_store(s, net_lane_of_cpu(cpu_current_id()));
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
 * pointer has to outlive its lock on its own -- see net_socket_ref() in
 * socket_internal.h.  This also closes a pre-existing hole: net_socket_from_file()
 * drops its vfile reference before returning, so a concurrent close() on another
 * CPU could free the socket before the caller got as far as taking a lock.
 */
void net_socket_free(net_socket_t *s) {
    if (!s)
        return;
    /*
     * The release decides: 1 means this was the last reference and the object
     * must be destroyed, 0 means another holder remains or the ledger refused
     * the drop (a double free, or a pointer that is not a live socket -- both
     * are counted and, under CONFIG_NET_REF_ASSERT, reported from there).
     *
     * The decision used to be inline here as a CAS loop over s->refs, because a
     * fetch_sub cannot tell "dropped the last reference" from "dropped one too
     * many": both land on the same atomic, and the second free walked straight
     * into obj_cache_free() with a negative count and no signal at all.
     * impl-notes-net.md §8.3/§8.6 called this the unverified primitive.  It now
     * belongs to the Rust registry state machine
     * (kernel/net/socket_registry.rs); what stays here is the destroy sequence,
     * which is primitives rather than state.
     */
    if (!net_sock_ref_release(s))
        return;
    if (s->ch_buf) {
        kfree(s->ch_buf);
        s->ch_buf = NULL;
    }
    resource_ns_put(s->net_ns);
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
    uint64_t timeout = net_socket_timeout_snapshot(s, for_write);
    return timeout && (int64_t)(timer_get_ticks() - (start + timeout)) >= 0;
}

uint64_t net_socket_timeout_snapshot(net_socket_t *s, int for_write) {
    if (!s)
        return 0;
    uint64_t flags = net_sock_lock(s);
    uint64_t timeout = for_write ? s->send_timeout_ticks : s->recv_timeout_ticks;
    net_sock_unlock(s, flags);
    return timeout;
}

int net_socket_wait_expired_locked(net_socket_t *s, uint64_t start,
                                   int for_write) {
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
    resource_namespace_t *net_ns;
    net_socket_t  *want;        /* bind-conflict only: the socket being bound */
    int            want_reuseaddr;
    int            want_reuseport;
    int            want_v6only;
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
    if ((a->domain != AF_UNIX || (a->addrlen > 2 && ((const uint8_t *)a->addr)[2] == 0)) &&
        s->net_ns != a->net_ns) return false;
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

net_socket_t *net_find_bound_socket(resource_namespace_t *ns, int domain, int type,
                                    const void *addr, size_t addrlen)
{
    net_find_arg_t a = {
        .net_ns = ns, .domain = domain, .type = type, .addr = addr, .addrlen = addrlen,
        .port = 0, .found = NULL,
    };
    if (domain != AF_UNIX && net_sockaddr_port(addr, addrlen, &a.port) < 0)
        return NULL;
    net_table_scan_all(net_find_bound_slot, &a);
    return a.found;
}

static int net_bind_sockets_overlap(const net_find_arg_t *a,
                                    const net_socket_t *b)
{
    int domain = a->want->domain;
    if (domain == b->domain)
        return 1;
    /* IPv4 vs IPv6 only conflicts when the IPv6 side is dual-stack.
     * A v6-only (IPV6_V6ONLY) listener shares the port with IPv4. */
    if (domain == AF_INET && b->domain == AF_INET6)
        return !b->ipv6_v6only;
    if (domain == AF_INET6 && b->domain == AF_INET)
        return !a->want_v6only;
    return 0;
}

static int net_bind_reuse_allowed(const net_find_arg_t *a,
                                 const net_socket_t *old_s)
{
    net_socket_t *new_s = a->want;
    if (!new_s || !old_s || new_s->type != old_s->type)
        return 0;
    if (new_s->type == SOCK_RAW)
        return new_s->protocol == old_s->protocol;
    if (new_s->type == SOCK_STREAM)
        return a->want_reuseport && old_s->reuseport;
    return (a->want_reuseaddr && old_s->reuseaddr) ||
           (a->want_reuseport && old_s->reuseport);
}

static bool net_find_conflict_slot(net_socket_t *s, int idx, void *arg)
{
    net_find_arg_t *a = arg;
    if (s->net_ns != a->want->net_ns) return false;
    (void)idx;
    if (s == a->want || !s->bound || s->type != a->want->type)
        return false;
    if (!net_bind_sockets_overlap(a, s))
        return false;
    if (!net_find_port_bound(s, s->domain, a->type, a->port))
        return false;
    if (net_bind_reuse_allowed(a, s))
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
    uint64_t flags = net_sock_lock(new_s);
    a.want_reuseaddr = new_s->reuseaddr;
    a.want_reuseport = new_s->reuseport;
    a.want_v6only = new_s->ipv6_v6only;
    net_sock_unlock(new_s, flags);
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
#if CONFIG_NET_LOCK_ASSERT
    /* Same reasoning as g_lwip_lock_armed: nothing above this line may be
     * judged, because the registry locks only exist from
     * net_socket_registry_init() on and no socket has been created yet.  After
     * arming, a violation of the net-lock contract is a panic rather than a
     * counter -- see kernel/net/net_lock_probe.c. */
    net_lock_probe_arm();
#endif
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

    /*
     * The reference-count ledger, next to the open-slot census because the two
     * answer the same question from opposite ends: `open` counts sockets the
     * table can still see, `live` counts sockets that exist at all -- including
     * the ones being created and the ones mid-teardown that have no slot.  A
     * create/destroy cycle that leaks moves `live` and not `open`, which is
     * exactly the pair of readings that used to be unavailable.
     */
    {
        char row[512];
        int r = net_sock_ref_format(row, sizeof(row));
        if (r > 0) {
            m = snprintf(buf + n, bufsz - (size_t)n, "%s", row);
            if (m > 0)
                n += m;
            if ((size_t)n >= bufsz)
                return (int)bufsz - 1;
        }
    }
    {
        char row[1024];
        int r = net_notconn_format(row, sizeof(row));
        if (r > 0) {
            m = snprintf(buf + n, bufsz - (size_t)n, "%s", row);
            if (m > 0)
                n += m;
            if ((size_t)n >= bufsz)
                return (int)bufsz - 1;
        }
    }
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
    /*
     * The SO_SNDBUF / SO_RCVBUF defaults are per type, not one pair for the
     * whole tree.  net_socket_alloc() seeded the stream values because it does
     * not know the type yet; a datagram socket has no pcb for either option to
     * mean anything about, so leaving it with TCP_SND_BUF / TCP_WND would both
     * misreport what it can honour and -- on the receive side -- silently bound
     * its queue at one window's worth of bytes.  See the NET_DGRAM_* block in
     * socket_internal.h for both numbers and for why the receive default is a
     * no-op against the message-count cap that already exists.
     */
    if (base_type == SOCK_STREAM || base_type == SOCK_SEQPACKET) {
        s->snd_buf = TCP_SND_BUF;
        s->rcv_buf = TCP_WND;
    } else {
        s->snd_buf = NET_DGRAM_SND_BUF_DEFAULT;
        s->rcv_buf = NET_DGRAM_RCV_BUF_DEFAULT;
    }

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
    /* Two sockets, taken in ascending address order.  Which registry buckets
     * they landed in is irrelevant now: the lock is the socket's own. */
    net_sock_pair_t pair = net_sock_lock2(sa, sb);
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
            int32_t pid = cur->tgid;
            int32_t uid = cur->cred.uid;
            int32_t gid = cur->cred.gid;
            sa->peer_pid = pid; sa->peer_uid = uid; sa->peer_gid = gid;
            sb->peer_pid = pid; sb->peer_uid = uid; sb->peer_gid = gid;
        }
    }
    net_sock_unlock2(pair);
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
            if ((!resource_ns_is_initial(s->net_ns) && in->sin_addr != 0 &&
                 ((const uint8_t *)&in->sin_addr)[0] != 127) ||
                (resource_ns_is_initial(s->net_ns) && !net_sockaddr_in_local(in)))
                return -EADDRNOTAVAIL;
            if (net_sockaddr_port(bind_addr, addrlen, &port) == 0 &&
                net_ntohs(port) < 1024 && net_ntohs(port) != 0) {
                task_t *cur = proc_current();
                if (cur && cur->cred.euid != 0)
                    return -EACCES;
            }
        }
        if (s->domain == AF_INET6 && !resource_ns_is_initial(s->net_ns)) {
            const net_sockaddr_in6_t *in = (const net_sockaddr_in6_t *)bind_addr;
            static const uint8_t any[16] = {0};
            static const uint8_t loopback[16] = {[15] = 1};
            if (memcmp(in->sin6_addr, any, 16) &&
                memcmp(in->sin6_addr, loopback, 16))
                return -EADDRNOTAVAIL;
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
    uint64_t flags = net_sock_lock(s);
    if (!net_socket_is_live(s)) {
        net_sock_unlock(s, flags);
        return -ENOTSOCK;
    }
    memcpy(s->local, bind_addr, bind_len);
    s->local_len = bind_len;
    s->bound = 1;
    net_sock_unlock(s, flags);
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

    uint64_t flags = net_sock_lock(s);
    if (!net_socket_is_live(s)) {
        net_sock_unlock(s, flags);
        return -ENOTSOCK;
    }
    if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
        net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
    memcpy(s->peer_addr, addr, addrlen);
    s->peer_len = addrlen;
    net_sock_unlock(s, flags);
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
    uint64_t irq = net_sock_lock(s);
    if (!net_socket_is_live(s)) {
        net_sock_unlock(s, irq);
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
    /* Copied out for the same reason src_local is: the loop below runs with no
     * lock on s at all. */
    int proto = s->protocol;
    net_sock_unlock(s, irq);

    /* One destination bucket at a time, and inside it one destination socket
     * lock at a time.  The slot table is still read under the bucket lock --
     * that is the one thing the bucket is for -- but the queue work, which is
     * what this loop spends its time on, is now serialized only against the
     * one socket being enqueued.  Before, a raw send on one socket blocked
     * every socket in the bucket, and holding s's own bucket across the whole
     * scan blocked all 512 of them at once.
     *
     * The wake batch is flushed when it fills -- exactly as the old single-lock
     * loop did when it filled -- so every destination is still visited and no
     * wake is dropped. */
    for (int bucket = 0; bucket < NET_SOCK_BUCKETS; bucket++) {
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t bf = net_bucket_lock(bucket);
        int base = bucket << NET_SOCK_BUCKET_SHIFT;
        for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
            net_socket_t *dst = g_sockets[base + k];
            if (!dst)
                continue;
            /* Bucket outer, socket inner: the same nesting net_bucket_scan()
             * does, and the only order a bucket lock may ever be taken in. */
            uint64_t df = net_sock_lock(dst);
            if (!dst->closed && dst->domain == AF_INET6 &&
                dst->type == SOCK_RAW && dst->protocol == proto &&
                raw_ipv6_filter_passes(dst, buf, len)) {
                if (net_enqueue_msg_locked(dst, buf, len, src_local,
                                           src_local_len) >= 0) {
                    delivered++;
                    (void)wait_queue_collect_one(
                        &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
                }
            }
            net_sock_unlock(dst, df);
            if (wake_q.count == PROC_WAKE_Q_CAPACITY)
                break;
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


/*
 * Primitives for the Rust sendto transaction (kernel/net/socket_sendto.rs).
 * Each one is a step the Rust decision surface cannot perform itself: a
 * per-family send entry, the netns test, a lock, the liveness test, the
 * reference and the free, the loopback autobind, the bound-socket scan, the
 * two enqueues, the read-wake collect or the wake queue.
 */
#include "net/socket_sendto_state.h"

_Static_assert(sizeof(net_sock_pair_t) <= sizeof(net_sendto_pair_t),
               "net_sendto_pair_t must mirror net_sock_pair_t");
_Static_assert(sizeof(proc_wake_q_t) <= 2048,
               "the Rust sendto frame must hold a proc_wake_q_t");

void a20_net_sendto_view_init(void *sp, net_sendto_view_t *out) {
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = (void **)&s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->protocol = &s->protocol;
    out->nonblock = &s->nonblock;
    out->closed = &s->closed;
    out->shut_wr = &s->shut_wr;
    out->bound = &s->bound;
    out->connected = &s->connected;
    out->ever_connected = &s->ever_connected;
    out->local_tcp = &s->local_tcp;
    out->tcp = (void **)&s->tcp;
    out->udp = (void **)&s->udp;
    out->raw = (void **)&s->raw;
    out->peer = (void **)&s->peer;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->peer_addr = s->peer_addr;
    out->peer_len = &s->peer_len;
    out->send_timeout_ticks = &s->send_timeout_ticks;
}

int a20_net_sendto_flags_check(int flags) { return net_msg_flags_check(flags); }

int a20_net_sendto_alg(void *sp, const void *buf, size_t len) {
    return net_alg_socket_send((net_socket_t *)sp, buf, len);
}

int a20_net_sendto_packet(void *sp, const void *buf, size_t len,
                          const void *addr, size_t addrlen) {
    return net_packet_socket_send((net_socket_t *)sp, buf, len, addr, addrlen);
}

/* The netlink protocol arms are pure dispatch, so they stay here. */
int a20_net_sendto_netlink(void *sp, const void *buf, size_t len,
                           const void *addr, size_t addrlen) {
    net_socket_t *s = (net_socket_t *)sp;
    if (s->protocol == NETLINK_SOCK_DIAG)
        return net_netlink_diag_request(s, buf, len, addr, addrlen);
    if (s->protocol == NETLINK_ROUTE)
        return net_netlink_route_request(s, buf, len, addr, addrlen);
    if (s->protocol == NETLINK_KOBJECT_UEVENT || s->protocol == NETLINK_GENERIC)
        return net_netlink_uevent_send(s, buf, len, addr, addrlen);
    return -EPROTONOSUPPORT;
}

int a20_net_sendto_ns_initial(void *sp) {
    return resource_ns_is_initial(((net_socket_t *)sp)->net_ns) ? 1 : 0;
}

int a20_net_sendto_raw_ipv6(void *sp, const void *buf, size_t len,
                            const void *addr, size_t addrlen) {
    return net_sendto_raw_ipv6((net_socket_t *)sp, (void *)buf, len, addr,
                               addrlen);
}

int a20_net_sendto_inet(void *sp, const void *buf, size_t len, int flags,
                        const void *addr, size_t addrlen) {
    return net_inet_sendto((net_socket_t *)sp, buf, len, flags, addr, addrlen);
}

int a20_net_sendto_notconn(void *sp) {
    (void)sp;
    return net_notconn(NET_NOTCONN_SENDTO_NOT_CONNECTED);
}

int a20_net_sendto_unix(void *sp, const void *buf, size_t len, const void *addr,
                        size_t addrlen) {
    return net_unix_socket_sendto((net_socket_t *)sp, buf, len, addr, addrlen);
}

uint64_t a20_net_sendto_lock(void *sp) { return net_sock_lock((net_socket_t *)sp); }

void a20_net_sendto_unlock(void *sp, uint64_t flags) {
    net_sock_unlock((net_socket_t *)sp, flags);
}

int a20_net_sendto_is_live(void *sp) {
    return net_socket_is_live((const net_socket_t *)sp) ? 1 : 0;
}

void a20_net_sendto_autobind(void *sp) {
    net_socket_t *s = (net_socket_t *)sp;
    net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
}

void *a20_net_sendto_ref(void *sp) { return net_socket_ref((net_socket_t *)sp); }

void a20_net_sendto_free(void *sp) { net_socket_free((net_socket_t *)sp); }

void a20_net_sendto_lock2(void *a, void *b, net_sendto_pair_t *out) {
    net_sock_pair_t p = net_sock_lock2((net_socket_t *)a, (net_socket_t *)b);
    out->lo = p.lo;
    out->hi = p.hi;
    out->flags = p.flags;
}

void a20_net_sendto_unlock2(const net_sendto_pair_t *in) {
    net_sock_pair_t p;
    p.lo = in->lo;
    p.hi = in->hi;
    p.flags = in->flags;
    net_sock_unlock2(p);
}

void *a20_net_sendto_find_bound(void *ns, int domain, int sock_type,
                                const void *addr, size_t addrlen) {
    return net_find_bound_socket((resource_namespace_t *)ns, domain, sock_type,
                                 addr, addrlen);
}

int a20_net_sendto_max_queue(void) { return NET_MAX_QUEUE; }

int a20_net_sendto_dst_rx_count(void *dst) {
    return ((net_socket_t *)dst)->rx_count;
}

int a20_net_sendto_enqueue_locked(void *dst, const void *buf, size_t len,
                                  const void *src, size_t src_len) {
    return net_enqueue_msg_locked((net_socket_t *)dst, buf, len, src, src_len);
}

int a20_net_sendto_enqueue_blocking(void *sp, void *dst, const void *buf,
                                    size_t len, const void *src, size_t src_len,
                                    int dontwait, uint64_t timeout) {
    return net_enqueue_msg_blocking((net_socket_t *)sp, (net_socket_t *)dst, buf,
                                    len, src, src_len, dontwait, timeout);
}

int a20_net_sendto_collect_read(void *dst, void *wake_q) {
    return wait_queue_collect_one(&((net_socket_t *)dst)->read_waitq, 0,
                                  PROC_WAKE_EVENT, (proc_wake_q_t *)wake_q);
}

void a20_net_sendto_wakeq_init(void *wake_q) {
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void a20_net_sendto_wakeq_flush(void *wake_q) {
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

int net_sendto_sock(net_socket_t *s, const void *buf, size_t len, int flags,
                    const void *addr, size_t addrlen) {
    return a20_net_sendto_sock(s, buf, len, flags, addr, addrlen);
}



/*
 * Primitives for the Rust blocking-receive transaction
 * (kernel/net/socket_recv.rs).  Each one is a single step the Rust decision
 * surface cannot perform itself: a lock, a copy out of the queue, a clock read,
 * a signal test, a park, a wake-queue collect or the lwIP receive accounting.
 * The lock discipline is Rust's -- it holds net_sock_lock across the decisions
 * and releases it before the wake-queue flush and before net_tcp_recved.
 */
#include "net/socket_recv_state.h"
#include "proc/park.h"

_Static_assert(sizeof(proc_wake_q_t) <= A20_NET_RECV_WAKEQ_BYTES,
               "A20_NET_RECV_WAKEQ_BYTES must hold a proc_wake_q_t");

void a20_net_recv_view_init(void *sp, net_recv_view_t *out) {
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->nonblock = &s->nonblock;
    out->closed = &s->closed;
    out->peer_closed = &s->peer_closed;
    out->shut_rd = &s->shut_rd;
    out->passcred = &s->passcred;
    out->recv_timeout_ticks = &s->recv_timeout_ticks;
    out->ch_ep = (void **)&s->ch_ep;
    out->rx_head = (void **)&s->rx_head;
    out->rx_count = &s->rx_count;
}

int a20_net_recv_flags_check(int flags) { return net_msg_flags_check(flags); }

int a20_net_recv_alg(void *sp, void *buf, size_t len) {
    return net_alg_socket_recv((net_socket_t *)sp, buf, len);
}

uint64_t a20_net_recv_now(void) { return timer_get_ticks(); }

void a20_net_recv_poll_waiter(void) { a20_lwip_poll_waiter(); }

uint64_t a20_net_recv_lock(void *sp) { return net_sock_lock((net_socket_t *)sp); }

void a20_net_recv_unlock(void *sp, uint64_t flags) {
    net_sock_unlock((net_socket_t *)sp, flags);
}

int a20_net_recv_have_task(void) { return proc_current() != NULL; }

int a20_net_recv_has_signal(void) {
    task_t *cur = proc_current();
    return cur ? net_task_has_unblocked_signal(cur) : 0;
}

int a20_net_recv_wait_expired(void *sp, uint64_t start, int for_write) {
    return net_socket_wait_expired((net_socket_t *)sp, start, for_write);
}

int a20_net_recv_wait_expired_locked(void *sp, uint64_t start, int for_write) {
    return net_socket_wait_expired_locked((net_socket_t *)sp, start, for_write);
}

size_t a20_net_recv_msg_avail(void *head) {
    net_msg_t *m = (net_msg_t *)head;
    return m->len - m->off;
}

int a20_net_recv_dequeue(void *sp, void *buf, size_t len, void *addr,
                         size_t *addrlen, void *meta) {
    return net_dequeue_msg_locked_meta((net_socket_t *)sp, buf, len, addr,
                                       addrlen, meta);
}

/*
 * The peek arm: copy the head's payload and, for a datagram, its address and
 * packet metadata, without unlinking the message.  Reports the message's full
 * payload length and the number of bytes copied so the caller can answer
 * MSG_TRUNC and MSG_PEEK without a second traversal.
 */
void a20_net_recv_peek_msg(void *head, void *buf, size_t len, void *addr,
                           size_t *addrlen, void *meta, size_t *out_avail,
                           size_t *out_n) {
    net_msg_t *m = (net_msg_t *)head;
    size_t avail = m->len - m->off;
    size_t n = avail < len ? avail : len;
    if (n)
        memcpy(buf, net_msg_payload(m) + m->off, n);
    if (addr && addrlen && *addrlen > 0) {
        size_t alen = m->addrlen < *addrlen ? m->addrlen : *addrlen;
        memcpy(addr, m->addr, alen);
        *addrlen = alen;
    }
    if (meta) {
        net_recv_meta_t *rm = (net_recv_meta_t *)meta;
        rm->has_pktinfo = m->has_pktinfo;
        rm->has_hoplimit = m->has_hoplimit;
        rm->has_tclass = m->has_tclass;
        rm->pktinfo_ifindex = m->pktinfo_ifindex;
        memcpy(rm->pktinfo_addr, m->pktinfo_addr, sizeof(rm->pktinfo_addr));
        rm->hoplimit = m->hoplimit;
        rm->tclass = m->tclass;
    }
    *out_avail = avail;
    *out_n = n;
}

int a20_net_recv_channel(void *sp, void *buf, size_t len, int peek) {
    net_socket_t *s = (net_socket_t *)sp;
    return peek ? unix_ch_peek(s, buf, len) : unix_ch_recv(s, buf, len);
}

/*
 * Channel-backed delivery has no net_msg metadata, so the sender's credentials
 * are stamped from the record unix_ch_send captured, and only when the receiver
 * asked for SCM_CREDENTIALS and the message did not already carry them.
 */
void a20_net_recv_creds(void *sp, void *meta) {
    net_socket_t *s = (net_socket_t *)sp;
    net_recv_meta_t *rm = (net_recv_meta_t *)meta;
    if (!rm || !s->passcred || rm->has_cred || !s->ch_ep)
        return;
    rm->has_cred = 1;
    rm->cred_pid = s->ch_cred_pid;
    rm->cred_uid = s->ch_cred_uid;
    rm->cred_gid = s->ch_cred_gid;
}

int a20_net_recv_collect_write(void *sp, void *wq) {
    net_socket_t *s = (net_socket_t *)sp;
    return wait_queue_collect_one(&s->write_waitq, 0, PROC_WAKE_EVENT,
                                  (proc_wake_q_t *)wq);
}

int a20_net_recv_collect_read(void *sp, void *wq) {
    net_socket_t *s = (net_socket_t *)sp;
    return wait_queue_collect_one(&s->read_waitq, 0, PROC_WAKE_EVENT,
                                  (proc_wake_q_t *)wq);
}

void a20_net_recv_wakeq_init(void *wq) { proc_wake_q_init((proc_wake_q_t *)wq); }

void a20_net_recv_wakeq_flush(void *wq) {
    (void)proc_wake_q_flush((proc_wake_q_t *)wq);
}

void a20_net_recv_recved(void *sp, size_t len) {
    net_tcp_recved((net_socket_t *)sp, len);
}

int a20_net_recv_park(void *sp, uint64_t wait_deadline, int channel,
                      int dontwait) {
    net_socket_t *s = (net_socket_t *)sp;
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, wait_deadline);
    if (!token.task)
        return -EAGAIN;
    wait_queue_entry_t entry = {0};
    uint64_t irq = net_sock_lock(s);
    if (channel) {
        /* Re-check before linking: a send between the empty check and the park
         * would otherwise wake nobody and we would sleep forever. */
        if (s->rx_head || a20_channel_readable(s->ch_ep)) {
            net_sock_unlock(s, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return 0;
        }
    } else {
        if (s->rx_head || s->nonblock || dontwait || s->closed ||
            s->peer_closed || s->shut_rd) {
            net_sock_unlock(s, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return 0;
        }
        if (net_task_has_unblocked_signal(proc_current())) {
            net_sock_unlock(s, irq);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -ERESTARTSYS;
        }
    }
    bool linked = wait_queue_link(&s->read_waitq, &entry, token, 0);
    net_sock_unlock(s, irq);
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
        return channel ? -EINTR : -ERESTARTSYS;
    /* The channel loop re-tests its deadline at the top, so a timeout is just
     * another retry there; the legacy loop reports it directly. */
    if (!channel && reason == PROC_WAKE_TIMEOUT)
        return -EAGAIN;
    return 0;
}

int net_recvfrom_socket_meta_until(net_socket_t *s, void *buf, size_t len,
                                   int flags, void *addr, size_t *addrlen,
                                   net_recv_meta_t *meta, uint64_t deadline) {
    return a20_net_recvfrom_socket_meta_until(s, buf, len, flags, addr, addrlen,
                                              meta, deadline);
}

int net_recvfrom_socket_meta(net_socket_t *s, void *buf, size_t len, int flags,
                             void *addr, size_t *addrlen,
                             net_recv_meta_t *meta)
{
    return net_recvfrom_socket_meta_until(s, buf, len, flags, addr, addrlen,
                                          meta, 0);
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
