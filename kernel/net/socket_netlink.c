#include "net/socket_internal.h"

#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
#include "proc/proc.h"
#include "drivers/core/driver_class.h"
#include "net/lwip_stack.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#define NLMSG_DONE              3
#define NLM_F_MULTI             0x2
#define SOCK_DIAG_BY_FAMILY     20
#define TCPDIAG_GETSOCK         18

#define UEVENT_GROUP           1
/* An uevent datagram is "ACTION@DEVPATH"; the emitter's own buffer is 256 B, so
 * anything longer cannot name a device this kernel publishes. */
#define UEVENT_MSG_MAX         256

#define TCP_ESTABLISHED         1
#define TCP_CLOSE               7
#define TCP_LISTEN              10

typedef struct netlink_msghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
} netlink_msghdr_t;

typedef struct inet_diag_sockid {
    uint16_t idiag_sport;
    uint16_t idiag_dport;
    uint32_t idiag_src[4];
    uint32_t idiag_dst[4];
    uint32_t idiag_if;
    uint32_t idiag_cookie[2];
} inet_diag_sockid_t;

typedef struct inet_diag_req_v2 {
    uint8_t sdiag_family;
    uint8_t sdiag_protocol;
    uint8_t idiag_ext;
    uint8_t pad;
    uint32_t idiag_states;
    inet_diag_sockid_t id;
} inet_diag_req_v2_t;

typedef struct inet_diag_msg {
    uint8_t idiag_family;
    uint8_t idiag_state;
    uint8_t idiag_timer;
    uint8_t idiag_retrans;
    inet_diag_sockid_t id;
    uint32_t idiag_expires;
    uint32_t idiag_rqueue;
    uint32_t idiag_wqueue;
    uint32_t idiag_uid;
    uint32_t idiag_inode;
} inet_diag_msg_t;

typedef struct inet_diag_reply {
    netlink_msghdr_t nlh;
    inet_diag_msg_t diag;
} inet_diag_reply_t;

typedef struct netlink_done {
    netlink_msghdr_t nlh;
    int32_t error;
} netlink_done_t;

static uint8_t net_diag_state(const net_socket_t *s)
{
    if (s->listening)
        return TCP_LISTEN;
    if (s->connected)
        return TCP_ESTABLISHED;
    return TCP_CLOSE;
}

static int net_diag_protocol_matches(const net_socket_t *s, uint8_t protocol)
{
    if (protocol == IPPROTO_TCP)
        return s->type == SOCK_STREAM;
    if (protocol == IPPROTO_UDP)
        return s->type == SOCK_DGRAM;
    return s->type == SOCK_RAW && s->protocol == protocol;
}

static void net_diag_copy_endpoint(const uint8_t *addr, size_t addrlen,
                                   uint16_t *port, uint32_t out[4])
{
    if (!addr || addrlen < sizeof(uint16_t))
        return;
    uint16_t family = *(const uint16_t *)addr;
    if (family == AF_INET && addrlen >= sizeof(net_sockaddr_in_t)) {
        const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)addr;
        *port = in->sin_port;
        out[0] = in->sin_addr;
    } else if (family == AF_INET6 && addrlen >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        *port = in6->sin6_port;
        memcpy(out, in6->sin6_addr, sizeof(in6->sin6_addr));
    }
}

static uint32_t net_diag_rx_bytes(const net_socket_t *s)
{
    uint64_t total = 0;
    for (const net_msg_t *m = s->rx_head; m; m = m->next)
        total += m->len - m->off;
    return total > 0xffffffffULL ? 0xffffffffU : (uint32_t)total;
}

int net_netlink_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s || !addr || addrlen < sizeof(net_sockaddr_nl_t))
        return -EINVAL;
    const net_sockaddr_nl_t *requested = (const net_sockaddr_nl_t *)addr;
    if (requested->nl_family != AF_NETLINK)
        return -EAFNOSUPPORT;

    net_sockaddr_nl_t local = *requested;
    if (local.nl_pid == 0) {
        task_t *cur = proc_current();
        local.nl_pid = cur ? (uint32_t)cur->pid : 1;
    }
    /* One socket, one bucket: publishing the bound address needs nothing else. */
    int sb = net_socket_bucket(s);
    uint64_t irq = net_bucket_lock(sb);
    if (!net_socket_is_live(s)) {
        net_bucket_unlock(sb, irq);
        return -ENOTSOCK;
    }
    memcpy(s->local, &local, sizeof(local));
    s->local_len = sizeof(local);
    s->bound = 1;
    net_bucket_unlock(sb, irq);

    /* udevd subscribes to the uevent multicast group when it starts; devices
     * registered before udevd ran had no listener, so their "add" uevents were
     * dropped.  Replay the current device set now that a listener exists so
     * coldplug works even though sysfs uevent files are read-only here. */
    if (s->protocol == NETLINK_KOBJECT_UEVENT &&
        (local.nl_groups & UEVENT_GROUP))
        class_device_emit_uevents("add");
    return 0;
}

int net_netlink_diag_request(net_socket_t *requester, const void *buf,
                             size_t len, const void *addr, size_t addrlen)
{
    if (!requester || requester->domain != AF_NETLINK ||
        requester->protocol != NETLINK_SOCK_DIAG)
        return -EPROTONOSUPPORT;
    if (!buf || len < sizeof(netlink_msghdr_t) + sizeof(inet_diag_req_v2_t))
        return -EINVAL;
    if (addr) {
        if (addrlen < sizeof(net_sockaddr_nl_t) ||
            ((const net_sockaddr_nl_t *)addr)->nl_family != AF_NETLINK)
            return -EAFNOSUPPORT;
    }

    const netlink_msghdr_t *req_nlh = (const netlink_msghdr_t *)buf;
    if (req_nlh->nlmsg_len < sizeof(netlink_msghdr_t) +
                              sizeof(inet_diag_req_v2_t) ||
        req_nlh->nlmsg_len > len ||
        (req_nlh->nlmsg_type != SOCK_DIAG_BY_FAMILY &&
         req_nlh->nlmsg_type != TCPDIAG_GETSOCK))
        return -EINVAL;
    const inet_diag_req_v2_t *req =
        (const inet_diag_req_v2_t *)((const uint8_t *)buf +
                                    sizeof(netlink_msghdr_t));

    net_sockaddr_nl_t kernel_addr = {
        .nl_family = AF_NETLINK,
        .nl_pid = 0,
        .nl_groups = 0,
    };
    /* One bucket at a time, each paired with the requester's own bucket so the
     * reply can be enqueued without ever holding a third lock.  Two buckets is
     * the documented ceiling, and net_bucket_lock2() takes them ascending, so
     * the scan cannot form a cycle with any other two-socket path. */
    int rb = net_socket_bucket(requester);
    uint32_t recipient_pid = 0;
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        net_bucket_pair_t pair = net_bucket_lock2(rb, b);
        if (pair.lo == rb)
            recipient_pid = requester->local_len >= sizeof(net_sockaddr_nl_t)
                ? ((const net_sockaddr_nl_t *)requester->local)->nl_pid : 0;
        int base = b << NET_SOCK_BUCKET_SHIFT;
        for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
        int i = base + k;
        net_socket_t *s = g_sockets[i];
        if (!s || s == requester ||
            (s->domain != AF_INET && s->domain != AF_INET6) ||
            (req->sdiag_family != AF_UNSPEC &&
             s->domain != req->sdiag_family) ||
            !net_diag_protocol_matches(s, req->sdiag_protocol))
            continue;

        uint8_t state = net_diag_state(s);
        if (req->idiag_states && !(req->idiag_states & (1U << state)))
            continue;

        inet_diag_reply_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.nlh.nlmsg_len = sizeof(reply);
        reply.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
        reply.nlh.nlmsg_flags = NLM_F_MULTI;
        reply.nlh.nlmsg_seq = req_nlh->nlmsg_seq;
        reply.nlh.nlmsg_pid = recipient_pid;
        reply.diag.idiag_family = (uint8_t)s->domain;
        reply.diag.idiag_state = state;
        net_diag_copy_endpoint(s->local, s->local_len,
                               &reply.diag.id.idiag_sport,
                               reply.diag.id.idiag_src);
        net_diag_copy_endpoint(s->peer_addr, s->peer_len,
                               &reply.diag.id.idiag_dport,
                               reply.diag.id.idiag_dst);
        reply.diag.id.idiag_cookie[0] = (uint32_t)(i + 1);
        reply.diag.id.idiag_cookie[1] = 0;
        reply.diag.idiag_rqueue = net_diag_rx_bytes(s);
        reply.diag.idiag_inode = (uint32_t)(i + 1);
        int r = net_enqueue_msg_locked(requester, &reply, sizeof(reply),
                                       &kernel_addr, sizeof(kernel_addr));
        if (r < 0) {
            net_bucket_unlock2(pair);
            return r;
        }
        }
        net_bucket_unlock2(pair);
    }

    netlink_done_t done;
    memset(&done, 0, sizeof(done));
    done.nlh.nlmsg_len = sizeof(done);
    done.nlh.nlmsg_type = NLMSG_DONE;
    done.nlh.nlmsg_flags = NLM_F_MULTI;
    done.nlh.nlmsg_seq = req_nlh->nlmsg_seq;
    done.nlh.nlmsg_pid = recipient_pid;
    /* The closing NLMSG_DONE needs the requester's bucket only. */
    uint64_t df = net_bucket_lock(rb);
    int r = net_enqueue_msg_locked(requester, &done, sizeof(done),
                                   &kernel_addr, sizeof(kernel_addr));
    net_bucket_unlock(rb, df);
    return r < 0 ? r : (int)len;
}

/* ------------------------------------------------------------------ */
/* NETLINK_KOBJECT_UEVENT (udev)                                       */
/* ------------------------------------------------------------------ */

/*
 * Broadcast a uevent to every netlink socket bound to KOBJECT_UEVENT with
 * multicast group 1 (the udev listener group).  The uevent datagram is the
 * raw "ACTION@DEVPATH" plus NUL-terminated KEY=VALUE pairs terminated by an
 * extra NUL, exactly the format libudev/udevd parse.
 *
 * Takes no lock of its own: the table walk is per-bucket and each listener is
 * enqueued into under the bucket that owns it, so this is safe with no bucket
 * lock held and is deliberately not callable from inside one.
 */
static int netlink_uevent_broadcast(const char *action,
                                    const char *subsystem,
                                    const char *name, uint64_t devt)
{
    unsigned major = (unsigned)((devt >> 8) & 0xffU);
    unsigned minor = (unsigned)(devt & 0xffU);

    /* Linux assigns every uevent a globally unique, strictly increasing
     * sequence number.  udevd's event ordering (event_queue_insert,
     * is_devpath_busy) relies on it: with all-seqnum-zero events the first
     * queued event is misjudged as "busy" (delaying_seqnum 0 == seqnum 0)
     * and no worker is ever spawned. */
    /* Atomic because the broadcast is no longer serialised by one global lock:
     * two CPUs emitting uevents now walk the table under different buckets and
     * would otherwise hand udevd the same sequence number twice, which is
     * exactly what the ordering comment below says it must not do. */
    static volatile uint64_t g_uevent_seq;
    uint64_t seqnum = __atomic_add_fetch(&g_uevent_seq, 1, __ATOMIC_RELAXED);

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "%s@/class/%s/%s%c"
        "SEQNUM=%llu%c"
        "ACTION=%s%c"
        "DEVPATH=/class/%s/%s%c"
        "SUBSYSTEM=%s%c"
        "MAJOR=%u%c"
        "MINOR=%u%c"
        "DEVNAME=%s/%s%c%c",
        action, subsystem, name, 0,
        (unsigned long long)seqnum, 0,
        action, 0,
        subsystem, name, 0,
        subsystem, 0,
        major, 0,
        minor, 0,
        subsystem, name, 0, 0);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return -EMSGSIZE;

    net_sockaddr_nl_t src = {
        .nl_family = AF_NETLINK, .nl_pid = 0, .nl_groups = UEVENT_GROUP,
    };
    int delivered = 0;
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        uint64_t bf = net_bucket_lock(b);
        int base = b << NET_SOCK_BUCKET_SHIFT;
        for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
            net_socket_t *s = g_sockets[base + k];
            if (!s || s->domain != AF_NETLINK ||
                s->protocol != NETLINK_KOBJECT_UEVENT || !s->bound)
                continue;
            net_sockaddr_nl_t *nl = (net_sockaddr_nl_t *)s->local;
            if (!nl || !(nl->nl_groups & UEVENT_GROUP))
                continue;
            int r = net_enqueue_msg_locked(s, buf, (size_t)n, &src, sizeof(src));
            if (r < 0) {
                net_bucket_unlock(b, bf);
                return r;
            }
            delivered++;
        }
        net_bucket_unlock(b, bf);
    }
    klog(KLOG_INFO,
         "[UEVENT] %s %s/%s devt=%u:%u delivered=%d\n",
         action, subsystem, name,
         (unsigned)((devt >> 8) & 0xffU), (unsigned)(devt & 0xffU),
         delivered);
    return 0;
}

/* Public entry: a kernel-side event for a published class device. */
void netlink_uevent_emit(const char *action, const char *subsystem,
                         const char *name, uint64_t devt)
{
    /* The broadcast walks the table one bucket at a time and enqueues into
     * each matching socket, so it takes no lock of its own here. */
    (void)netlink_uevent_broadcast(action, subsystem, name, devt);
}

/* The verbs the kobject uevent layer accepts (kernel/ksysfs.c
 * uevent_trigger).  Anything else is a caller bug and is refused, because
 * inventing a valid-looking action would publish a false device event. */
static int uevent_action_known(const char *action, size_t n)
{
    static const char *const known[] = {
        "add", "remove", "delete", "change", "move",
        "bind", "unbind", "online", "offline", "rename",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strlen(known[i]) == n && memcmp(known[i], action, n) == 0)
            return 1;
    return 0;
}

/*
 * Handle send() on a KOBJECT_UEVENT socket.  The datagram is the sysfs uevent
 * write format: "ACTION@DEVPATH" (or a bare "ACTION"), exactly what
 * uevent_trigger() parses.  The action is delivered as the caller wrote it and
 * DEVPATH selects one device; a bare action replays the whole published set,
 * which is what udevadm trigger does.  Returns the byte count only when the
 * event really was emitted.
 */
int net_netlink_uevent_send(net_socket_t *requester, const void *buf,
                            size_t len, const void *addr, size_t addrlen)
{
    if (!requester || requester->domain != AF_NETLINK ||
        requester->protocol != NETLINK_KOBJECT_UEVENT)
        return -EPROTONOSUPPORT;
    (void)addr;
    (void)addrlen;
    if (!buf || len == 0 || len > UEVENT_MSG_MAX)
        return -EINVAL;

    char msg[UEVENT_MSG_MAX + 1];
    memcpy(msg, buf, len);
    msg[len] = '\0';               /* a datagram carries no terminator */

    char *at = strchr(msg, '@');
    size_t action_len = at ? (size_t)(at - msg) : strlen(msg);
    if (action_len == 0 || !uevent_action_known(msg, action_len))
        return -EINVAL;
    msg[action_len] = '\0';
    const char *action = msg;
    const char *devpath = at ? at + 1 : NULL;

    if (!devpath) {
        class_device_emit_uevents(action);
        return (int)len;
    }

    /* Our own emitter writes DEVPATH as /class/<subsystem>/<name>, and the
     * class registry is keyed by that <name>. */
    const char *slash = strrchr(devpath, '/');
    const char *name = slash ? slash + 1 : devpath;
    if (!*name)
        return -EINVAL;
    class_device_t *cdev = class_device_get_by_name(name);
    if (!cdev)
        return -ENOENT;
    const char *subsystem = class_device_subsystem(cdev->class_type);
    if (subsystem)
        netlink_uevent_emit(action, subsystem, cdev->name, cdev->devt);
    int rc = subsystem ? 0 : -EINVAL;
    class_device_put(cdev);
    return rc < 0 ? rc : (int)len;
}

/* ------------------------------------------------------------------ */
/* NETLINK_ROUTE (ip / ifconfig / NetworkManager)                       */
/* ------------------------------------------------------------------ */

#define RTM_NEWLINK      16
#define RTM_GETLINK      18
#define RTM_NEWADDR      20
#define RTM_DELADDR      21
#define RTM_GETADDR      22
#define RTM_GETROUTE     26

#define IFLA_ADDRESS     1
#define IFLA_IFNAME      3
#define IFLA_MTU         4
#define IFLA_OPERSTATE   5

#define IFA_ADDRESS      1
#define IFA_LOCAL        2
#define IFA_LABEL        3
#define IFA_BROADCAST    5

#define RTA_GATEWAY      3
#define RTA_OIF          4

#define NLMSG_ALIGNTO    4
#define NLM_F_REPLACE    0x100   /* RTM_NEWADDR may reconfigure a netif that
                                  * already holds a different address */
#define NLRT_IF_FLAGS_UP 0x1
#define NLRT_IF_FLAGS_LOOPBACK 0x8
#define NLRT_IF_FLAGS_RUNNING  0x40
#define NLRT_RTPROT_BOOT 3
#define NLRT_SCOPE_UNIVERSE 0
#define NLRT_MAX_LINKS   8
#define NLRT_MSG_MAX     256
#define NLRT_MIN_MTU     68      /* RFC 791 link minimum */

typedef struct {
    uint8_t  ifi_family;
    uint8_t  ifi_pad;
    uint16_t ifi_type;
    int32_t  ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
} ifinfomsg_t;

typedef struct {
    uint8_t  ifa_family;
    uint8_t  ifa_prefixlen;
    uint8_t  ifa_flags;
    uint8_t  ifa_scope;
    uint32_t ifa_index;
} ifaddrmsg_t;

typedef struct {
    uint8_t  rtm_family, rtm_dst_len, rtm_src_len, rtm_tos;
    uint8_t  rtm_table, rtm_protocol, rtm_scope, rtm_type;
    uint32_t rtm_flags;
} rtmsg_t;

typedef struct {
    uint16_t rta_len;
    uint16_t rta_type;
} rtattr_t;

_Static_assert(sizeof(ifinfomsg_t) == 16, "ifinfomsg wire layout");
_Static_assert(sizeof(ifaddrmsg_t) == 8, "ifaddrmsg wire layout");
_Static_assert(sizeof(rtmsg_t) == 12, "rtmsg wire layout");
_Static_assert(sizeof(rtattr_t) == 4, "rtattr wire layout");

/* One netif's user-visible state, copied out from under g_lwip_lock. */
typedef struct {
    char     name[16];
    uint32_t index;
    uint32_t mtu;
    uint32_t flags;
    uint8_t  mac[6];
    uint8_t  loopback;
    uint8_t  has_ip, has_gw;
    uint8_t  ip[4], mask[4], gw[4];
} nlrt_link_t;

static void nlrt_copy_ip4(uint8_t out[4], const ip4_addr_t *a)
{
    out[0] = ip4_addr1(a);
    out[1] = ip4_addr2(a);
    out[2] = ip4_addr3(a);
    out[3] = ip4_addr4(a);
}

/*
 * ifa_prefixlen is the count of leading one bits in the netmask.  Deriving it
 * that way matters: a `mask[0] ? 24 : 0` shortcut publishes a /8 interface as
 * /24, and `ip addr show` then repeats the fiction.  A mask with a hole in it
 * is not a netmask at all; nothing here can produce one, so the leading-one
 * count is reported rather than a plausible-looking guess.
 */
static uint8_t nlrt_mask_prefixlen(const uint8_t mask[4])
{
    unsigned n = 0;
    for (unsigned i = 0; i < 4; i++)
        for (int bit = 7; bit >= 0; bit--) {
            if (!(mask[i] & (1u << bit)))
                return (uint8_t)n;
            n++;
        }
    return 32;
}

/*
 * Copy the interface list out of lwIP.  The lock contract forbids holding
 * g_lwip_lock together with a socket-table bucket lock, so the snapshot is
 * taken and the lwIP
 * lock dropped before any reply is enqueued.
 */
static int nlrt_snapshot(nlrt_link_t *out, int max)
{
    uint64_t lf = a20_lwip_lock();
    int n = 0;
    for (struct netif *ni = netif_list; ni && n < max; ni = ni->next) {
        nlrt_link_t *e = &out[n++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%c%c%u",
                 ni->name[0], ni->name[1], ni->num);
        e->index = (uint32_t)netif_get_index(ni);
        e->mtu = ni->mtu;
        e->flags = NLRT_IF_FLAGS_LOOPBACK;
        if (netif_is_up(ni))
            e->flags |= NLRT_IF_FLAGS_UP;
        if (netif_is_link_up(ni))
            e->flags |= NLRT_IF_FLAGS_RUNNING;
        if (ni->flags & NETIF_FLAG_ETHERNET)
            e->loopback = 0, e->flags &= ~(uint32_t)NLRT_IF_FLAGS_LOOPBACK;
        else
            e->loopback = 1;
        memcpy(e->mac, ni->hwaddr, 6);
        const ip4_addr_t *ip = netif_ip4_addr(ni);
        const ip4_addr_t *mask = netif_ip4_netmask(ni);
        const ip4_addr_t *gw = netif_ip4_gw(ni);
        if (ip && !ip4_addr_isany_val(*ip)) {
            e->has_ip = 1;
            nlrt_copy_ip4(e->ip, ip);
        }
        if (mask)
            nlrt_copy_ip4(e->mask, mask);
        if (gw && !ip4_addr_isany_val(*gw)) {
            e->has_gw = 1;
            nlrt_copy_ip4(e->gw, gw);
        }
    }
    a20_lwip_unlock(lf);
    return n;
}

/* Append one rtattr, padding the payload to NLMSG_ALIGNTO. */
static size_t nlrt_put_attr(uint8_t *buf, size_t off, uint16_t type,
                            const void *data, size_t len)
{
    size_t need = sizeof(rtattr_t) + len;
    size_t padded = (need + (NLMSG_ALIGNTO - 1)) & ~(size_t)(NLMSG_ALIGNTO - 1);
    if (off + padded > NLRT_MSG_MAX)
        return off;
    rtattr_t *a = (rtattr_t *)(buf + off);
    a->rta_len = (uint16_t)need;
    a->rta_type = type;
    if (len)
        memcpy(buf + off + sizeof(rtattr_t), data, len);
    memset(buf + off + need, 0, padded - need);
    return off + padded;
}

static void nlrt_fill_hdr(netlink_msghdr_t *nlh, uint16_t type, uint32_t total,
                          uint32_t seq, uint32_t pid)
{
    nlh->nlmsg_len = total;
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = NLM_F_MULTI;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = pid;
}

/*
 * Build one RTM_NEWLINK/RTM_GETLINK-shaped message from a snapshot entry.
 * Returns the total nlmsg_len, or 0 if the payload does not fit NLRT_MSG_MAX.
 *
 * Factored out of the RTM_GETLINK dump so a multicast notification is built by
 * the same code as the dump.  A listener that diffs "the link I was told about"
 * against "the link in the next dump" would otherwise be comparing two
 * independently written encodings, and any drift between them shows up as a
 * phantom change rather than as a bug.
 */
static size_t nlrt_build_linkmsg(uint8_t *buf, const nlrt_link_t *e,
                                 uint16_t msg_type, uint32_t pid,
                                 uint32_t seq)
{
    size_t off = sizeof(netlink_msghdr_t);
    ifinfomsg_t *ifi = (ifinfomsg_t *)(buf + off);
    memset(ifi, 0, sizeof(*ifi));
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = (int32_t)e->index;
    ifi->ifi_flags = e->flags;
    ifi->ifi_change = 0xffffffffU;
    off += sizeof(*ifi);
    if (e->mtu)
        off = nlrt_put_attr(buf, off, IFLA_MTU, &e->mtu, sizeof(e->mtu));
    off = nlrt_put_attr(buf, off, IFLA_ADDRESS, e->mac, sizeof(e->mac));
    off = nlrt_put_attr(buf, off, IFLA_IFNAME, e->name, strlen(e->name) + 1);
    size_t total = sizeof(netlink_msghdr_t) + off;
    if (total > NLRT_MSG_MAX)
        return 0;
    nlrt_fill_hdr((netlink_msghdr_t *)buf, msg_type, (uint32_t)total,
                  seq, pid);
    return total;
}

/*
 * Build one RTM_NEWADDR/RTM_GETADDR-shaped message.  Same reason as above.
 *
 * `addr` / `mask` are passed rather than taken from the snapshot entry because
 * the caller holds the values it just applied (or, for a delete, the values it
 * just removed).  Reading them back from a post-change snapshot would report an
 * all-zero address for a delete, which is not an address anyone can act on.
 * `msg_type` selects RTM_NEWADDR vs RTM_DELADDR; the payload is identical,
 * which is what Linux does too -- the type is the whole difference.
 */
static size_t nlrt_build_addrmsg(uint8_t *buf, uint32_t ifindex,
                                 const uint8_t addr[4], const uint8_t mask[4],
                                 const char *name, uint8_t loopback,
                                 uint16_t msg_type,
                                 uint32_t pid, uint32_t seq)
{
    size_t off = sizeof(netlink_msghdr_t);
    ifaddrmsg_t *ifa = (ifaddrmsg_t *)(buf + off);
    memset(ifa, 0, sizeof(*ifa));
    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = nlrt_mask_prefixlen(mask);
    ifa->ifa_scope = loopback ? 254 : NLRT_SCOPE_UNIVERSE;
    ifa->ifa_index = ifindex;
    off += sizeof(*ifa);
    off = nlrt_put_attr(buf, off, IFA_ADDRESS, addr, 4);
    off = nlrt_put_attr(buf, off, IFA_LOCAL, addr, 4);
    if (name && name[0])
        off = nlrt_put_attr(buf, off, IFA_LABEL, name, strlen(name) + 1);
    size_t total = sizeof(netlink_msghdr_t) + off;
    if (total > NLRT_MSG_MAX)
        return 0;
    nlrt_fill_hdr((netlink_msghdr_t *)buf, msg_type, (uint32_t)total,
                  seq, pid);
    return total;
}

/* Bounded rtattr walker over a user-supplied attribute block.  The payload is
 * only ever bounded by the validated nlmsg_len, and a length that does not fit
 * the remaining block is a parse error, not a reason to stop early. */
typedef struct {
    const uint8_t *base;
    size_t len;
    size_t off;
    int bad;
} nlrt_attr_iter_t;

static const rtattr_t *nlrt_attr_next(nlrt_attr_iter_t *it)
{
    if (it->bad || it->off >= it->len)
        return NULL;
    size_t avail = it->len - it->off;
    if (avail < sizeof(rtattr_t)) {
        it->bad = 1;
        return NULL;
    }
    const rtattr_t *a = (const rtattr_t *)(it->base + it->off);
    size_t alen = a->rta_len;
    size_t padded = (alen + (NLMSG_ALIGNTO - 1)) & ~(size_t)(NLMSG_ALIGNTO - 1);
    if (alen < sizeof(rtattr_t) || alen > avail || padded > avail) {
        it->bad = 1;
        return NULL;
    }
    it->off += padded;
    return a;
}

/* Fetch a fixed-size attribute payload, or -EINVAL if this attribute is not
 * exactly that many bytes. */
static int nlrt_attr_get(const rtattr_t *a, size_t want, void *out)
{
    if (a->rta_len != sizeof(rtattr_t) + want)
        return -EINVAL;
    memcpy(out, (const uint8_t *)a + sizeof(rtattr_t), want);
    return 0;
}

/*
 * RTM_NEWLINK: ifi_change/ifi_flags carry the admin state (the kernel's own
 * rule is `if (ifi_change & IFF_UP) want = ifi_flags & IFF_UP`), IFLA_MTU the
 * link MTU.  Everything is validated before anything is applied, and the
 * helpers are ordered so the only failure after the first is an unknown
 * ifindex -- which the first helper called already reports.
 */
static int nlrt_apply_newlink(const ifinfomsg_t *ifi, const uint8_t *attrs,
                              size_t alen)
{
    if (ifi->ifi_index <= 0)
        return -EINVAL;
    unsigned ifindex = (unsigned)ifi->ifi_index;
    /* IFF_UP is the only flag a netif here can be told about.  Honouring the
     * rest silently would answer success for a change that never happened. */
    if (ifi->ifi_change & ~(uint32_t)NLRT_IF_FLAGS_UP)
        return -EOPNOTSUPP;

    int have_mtu = 0;
    uint32_t mtu = 0;
    int want_up = 0;
    nlrt_attr_iter_t it = { attrs, alen, 0, 0 };
    const rtattr_t *a;
    while ((a = nlrt_attr_next(&it))) {
        if (a->rta_type == IFLA_MTU) {
            int r = nlrt_attr_get(a, sizeof(mtu), &mtu);
            if (r < 0)
                return r;
            have_mtu = 1;
        } else if (a->rta_type == IFLA_OPERSTATE) {
            /* Carrier belongs to the driver: a20_lwip_sync_link_state()
             * re-derives it on every poll, so a user-written operstate would
             * be reverted on the next tick.  Refuse instead of reporting a
             * change that cannot stick. */
            return -EOPNOTSUPP;
        }
        /* IFLA_IFNAME / IFLA_ADDRESS identify the link; there is exactly one
         * netif per index here, so neither is a change. */
    }
    if (it.bad)
        return -EINVAL;
    if (have_mtu && (mtu < NLRT_MIN_MTU || mtu > 0xffff))
        return -EINVAL;
    if (ifi->ifi_change & (uint32_t)NLRT_IF_FLAGS_UP)
        want_up = (ifi->ifi_flags & (uint32_t)NLRT_IF_FLAGS_UP) ? 1 : 0;

    int rc = 0;
    if (have_mtu) {
        rc = a20_lwip_if_set_mtu(ifindex, (uint16_t)mtu);
        if (rc < 0)
            return rc;
    }
    if (ifi->ifi_change & (uint32_t)NLRT_IF_FLAGS_UP)
        rc = a20_lwip_if_set_flags(ifindex,
                                    want_up ? NLRT_IF_FLAGS_UP : 0,
                                    NLRT_IF_FLAGS_UP);
    return rc;
}

static int nlrt_ip4_isany(const uint8_t a[4])
{
    return a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0;
}

/*
 * RTM_NEWADDR / RTM_DELADDR.  The address is IFA_LOCAL, falling back to
 * IFA_ADDRESS; the netmask comes from ifa_prefixlen because this message
 * carries no mask attribute.  A NULL gw leaves the gateway alone: RTM_NEWADDR
 * has no way to spell one.
 */
static int nlrt_apply_addr(uint16_t type, uint16_t flags,
                           const ifaddrmsg_t *ifa,
                           const uint8_t *attrs, size_t alen)
{
    if (ifa->ifa_family != AF_INET)
        return -EAFNOSUPPORT;      /* no IPv6 address write path */
    if (ifa->ifa_index == 0)
        return -EINVAL;
    if (ifa->ifa_prefixlen > 32)
        return -EINVAL;
    unsigned ifindex = ifa->ifa_index;

    uint8_t address[4], local[4];
    int have_address = 0, have_local = 0;
    nlrt_attr_iter_t it = { attrs, alen, 0, 0 };
    const rtattr_t *a;
    while ((a = nlrt_attr_next(&it))) {
        if (a->rta_type == IFA_ADDRESS) {
            int r = nlrt_attr_get(a, sizeof(address), address);
            if (r < 0)
                return r;
            have_address = 1;
        } else if (a->rta_type == IFA_LOCAL) {
            int r = nlrt_attr_get(a, sizeof(local), local);
            if (r < 0)
                return r;
            have_local = 1;
        }
    }
    if (it.bad)
        return -EINVAL;

    if (have_local && have_address && memcmp(local, address, 4) != 0) {
        /* A distinct IFA_LOCAL is how a secondary address is spelled (it is the
         * point-to-point form: IFA_ADDRESS is then the peer).  lwIP has one
         * IPv4 slot per netif and no secondary-address API, so applying this
         * would overwrite the primary while answering success. */
        return -EOPNOTSUPP;
    }
    const uint8_t *want;
    if (have_local)
        want = local;
    else if (have_address)
        want = address;
    else
        return -EINVAL;

    uint8_t cur[4], cur_mask[4], cur_gw[4];
    int rc = a20_lwip_if_get_addr(ifindex, cur, cur_mask, cur_gw);
    if (rc < 0)
        return rc;                  /* unknown ifindex, reported before any write */

    if (type == RTM_DELADDR) {
        /* Confirm the address is the one configured before clearing the slot,
         * so a stale delete cannot remove a configuration the caller never
         * named. */
        if (nlrt_ip4_isany(cur) || memcmp(cur, want, 4) != 0)
            return -EADDRNOTAVAIL;
        const uint8_t none[4] = { 0, 0, 0, 0 };
        return a20_lwip_if_set_addr(ifindex, none, NULL, NULL);
    }

    /* RTM_NEWADDR.  One IPv4 slot per netif, so a netif already holding a
     * *different* address can only be reconfigured by an explicit
     * NLM_F_REPLACE (ip addr replace), never silently overwritten by a plain
     * add.  The read and the write are separate lock acquisitions because the
     * lock contract forbids holding g_lwip_lock and a socket-table bucket lock
     * together, and
     * this stack has no rtnl_lock equivalent; two config requests racing on
     * one interface are therefore last-writer-wins. */
    if (!nlrt_ip4_isany(cur) && memcmp(cur, want, 4) != 0 &&
        !(flags & NLM_F_REPLACE))
        return -EOPNOTSUPP;

    uint8_t mask[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < ifa->ifa_prefixlen; i++)
        mask[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    /* IFA_BROADCAST / IFA_LABEL are accepted and unused: this stack derives the
     * broadcast address from the netmask, and an interface has exactly one
     * address, so there is no second label to move. */
    return a20_lwip_if_set_addr(ifindex, want, mask, NULL);
}

int net_netlink_route_request(net_socket_t *requester, const void *buf,
                              size_t len, const void *addr, size_t addrlen)
{
    if (!requester || requester->domain != AF_NETLINK ||
        requester->protocol != NETLINK_ROUTE)
        return -EPROTONOSUPPORT;
    if (!buf || len < sizeof(netlink_msghdr_t))
        return -EINVAL;
    /* musl's send() passes a pointer to a zeroed sockaddr with addrlen 0, so an
     * absent address looks like a present one; only validate a real length. */
    if (addr && addrlen >= sizeof(net_sockaddr_nl_t) &&
        ((const net_sockaddr_nl_t *)addr)->nl_family != AF_NETLINK)
        return -EAFNOSUPPORT;

    const netlink_msghdr_t *req = (const netlink_msghdr_t *)buf;
    size_t payload = len - sizeof(netlink_msghdr_t);
    uint16_t type = req->nlmsg_type;
    size_t want;
    if (type == RTM_GETLINK || type == RTM_NEWLINK)
        want = sizeof(ifinfomsg_t);
    else if (type == RTM_GETADDR || type == RTM_NEWADDR ||
             type == RTM_DELADDR)
        want = sizeof(ifaddrmsg_t);
    else if (type == RTM_GETROUTE)
        want = sizeof(rtmsg_t);
    else
        return -EOPNOTSUPP;
    if (req->nlmsg_len < sizeof(netlink_msghdr_t) + want ||
        req->nlmsg_len > len || payload < want)
        return -EINVAL;

    /* Write requests run before any reply is built and hold no lock: the lwIP
     * helpers take g_lwip_lock themselves, and the lock contract forbids
     * holding it together with a socket-table bucket lock (taken only further
     * down). */
    if (type == RTM_NEWLINK || type == RTM_NEWADDR || type == RTM_DELADDR) {
        /* Only a single-message write is honoured; a multipart request would
         * carry further nlmsghdrs this path does not walk. */
        if (req->nlmsg_flags & NLM_F_MULTI)
            return -EINVAL;
        const uint8_t *body = (const uint8_t *)buf + sizeof(*req);
        size_t blen = (size_t)req->nlmsg_len - sizeof(*req) - want;
        if (type == RTM_NEWLINK)
            return nlrt_apply_newlink((const ifinfomsg_t *)body, body + want,
                                      blen);
        return nlrt_apply_addr(type, req->nlmsg_flags,
                               (const ifaddrmsg_t *)body, body + want, blen);
    }

    nlrt_link_t links[NLRT_MAX_LINKS];
    int nlinks = nlrt_snapshot(links, NLRT_MAX_LINKS);

    net_sockaddr_nl_t from = { .nl_family = AF_NETLINK, .nl_pid = 0, .nl_groups = 0 };
    /* Every reply below is enqueued into the requester, and nothing else is
     * touched, so the requester's own bucket is the only lock this needs. */
    int rb = net_socket_bucket(requester);
    uint64_t irq = net_bucket_lock(rb);
    if (!net_socket_is_live(requester)) {
        net_bucket_unlock(rb, irq);
        return -ENOTSOCK;
    }
    uint32_t pid = requester->local_len >= sizeof(net_sockaddr_nl_t)
        ? ((const net_sockaddr_nl_t *)requester->local)->nl_pid : 0;

    union { uint64_t align; uint8_t b[NLRT_MSG_MAX]; } msg;
    int rc = 0;

    for (int i = 0; i < nlinks && rc >= 0; i++) {
        const nlrt_link_t *e = &links[i];
        /* The builders share the encoding with the multicast path; `type` is
         * the only thing that differs between a dump and a notification. */
        size_t total;
        if (type == RTM_GETLINK) {
            total = nlrt_build_linkmsg(msg.b, e, RTM_GETLINK, pid,
                                       req->nlmsg_seq);
        } else if (type == RTM_GETADDR) {
            if (!e->has_ip)
                continue;
            total = nlrt_build_addrmsg(msg.b, e->index, e->ip, e->mask,
                                       e->name, e->loopback, RTM_GETADDR,
                                       pid, req->nlmsg_seq);
        } else {
            if (!e->has_gw)
                continue;
            size_t off = sizeof(netlink_msghdr_t);
            rtmsg_t *rt = (rtmsg_t *)(msg.b + off);
            memset(rt, 0, sizeof(*rt));
            rt->rtm_family = AF_INET;
            rt->rtm_dst_len = 0;
            rt->rtm_table = 254;             /* RT_TABLE_MAIN */
            rt->rtm_protocol = NLRT_RTPROT_BOOT;
            rt->rtm_scope = NLRT_SCOPE_UNIVERSE;
            rt->rtm_type = 1;                /* RTN_UNICAST */
            off += sizeof(*rt);
            off = nlrt_put_attr(msg.b, off, RTA_GATEWAY, e->gw, sizeof(e->gw));
            uint32_t oif = e->index;
            off = nlrt_put_attr(msg.b, off, RTA_OIF, &oif, sizeof(oif));
            total = sizeof(netlink_msghdr_t) + off;
            nlrt_fill_hdr((netlink_msghdr_t *)msg.b, type, (uint32_t)total,
                          req->nlmsg_seq, pid);
        }
        if (total == 0)
            continue;               /* nlrt_put_attr refused; see NLRT_MSG_MAX */
        rc = net_enqueue_msg_locked(requester, msg.b, total, &from, sizeof(from));
    }

    if (rc >= 0) {
        netlink_done_t done;
        memset(&done, 0, sizeof(done));
        nlrt_fill_hdr(&done.nlh, NLMSG_DONE, (uint32_t)sizeof(done),
                      req->nlmsg_seq, pid);
        rc = net_enqueue_msg_locked(requester, &done, sizeof(done),
                                    &from, sizeof(from));
    }
    net_bucket_unlock(rb, irq);
    return rc < 0 ? rc : (int)len;
}

/* ------------------------------------------------------------------ */
/* RTNETLINK multicast: RTM_NEWLINK / RTM_NEWADDR                     */
/* ------------------------------------------------------------------ */

/* Group numbers from linux/rtnetlink.h.  RTNLGRP_LINK is what NetworkManager
 * and `ip monitor link` bind to see carrier transitions; RTNLGRP_IPV4_IFADDR
 * is the address group.  A listener binds the bitwise OR of the groups it
 * wants, exactly as it already does for the uevent group. */
#define RTNLGRP_LINK        0x1
#define RTNLGRP_IPV4_IFADDR 0x5

/*
 * Sequence numbers for notifications.  Atomic because the broadcast is not
 * serialised by any single lock: two CPUs flushing pending link events walk
 * the table under different bucket locks, and a repeated nlmsg_seq is what
 * lets a listener's reorder logic mistake a fresh event for a retransmit.
 */
static volatile uint32_t g_nlrt_notify_seq;

static uint32_t nlrt_next_seq(void)
{
    return __atomic_add_fetch(&g_nlrt_notify_seq, 1, __ATOMIC_RELAXED);
}

/*
 * Hand one already-built message to every NETLINK_ROUTE socket bound to
 * `group`.
 *
 * Takes no lock of its own beyond the one bucket at a time, and must be called
 * with g_lwip_lock NOT held: the lock contract forbids holding it together with
 * a socket-table bucket lock, and nlrt_snapshot() takes it for itself.
 *
 * A full receive queue for one listener does not stop the others: Linux drops
 * the message for that socket and reports the overflow, because the
 * alternative -- abandoning the broadcast -- would let one slow reader silence
 * every other reader.  There is no socket-level "report the drop" surface here,
 * so this counts it and moves on; see the klog below.
 */
static int nlrt_broadcast(const uint8_t *msg, size_t len, uint32_t group)
{
    net_sockaddr_nl_t src = {
        .nl_family = AF_NETLINK, .nl_pid = 0, .nl_groups = group,
    };
    int delivered = 0, dropped = 0;
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        uint64_t bf = net_bucket_lock(b);
        int base = b << NET_SOCK_BUCKET_SHIFT;
        for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
            net_socket_t *s = g_sockets[base + k];
            if (!s || s->domain != AF_NETLINK ||
                s->protocol != NETLINK_ROUTE || !s->bound)
                continue;
            net_sockaddr_nl_t *nl = (net_sockaddr_nl_t *)s->local;
            if (!nl || !(nl->nl_groups & group))
                continue;
            if (net_enqueue_msg_locked(s, msg, len, &src, sizeof(src)) < 0)
                dropped++;
            else
                delivered++;
        }
        net_bucket_unlock(b, bf);
    }
    /* A silent drop is the one failure mode a listener can never diagnose:
     * it sees a gap in the event stream with nothing to explain it.  There is
     * no per-socket overflow report on this stack to surface it in, so it is
     * logged here instead. */
    if (dropped)
        klog(KLOG_WARN,
             "[RTNETLINK] multicast group 0x%x dropped for %d listener(s), delivered=%d\n",
             (unsigned)group, dropped, delivered);
    return dropped ? -EAGAIN : (delivered ? 0 : -ENOENT);
}

/*
 * A link's admin/carrier state changed (a20_lwip_sync_link_state() saw the
 * driver flip), or user space asked for a change through RTM_NEWLINK.  Publish
 * the link's post-change state to RTNLGRP_LINK.
 *
 * `events` is the batch a20_lwip_netlink_flush() collected while holding
 * g_lwip_lock; it is replayed here with the lock released.
 */
void net_netlink_link_notify(const nlrt_link_event_t *events, int n)
{
    if (!events || n <= 0)
        return;
    nlrt_link_t links[NLRT_MAX_LINKS];
    int nlinks = nlrt_snapshot(links, NLRT_MAX_LINKS);
    if (nlinks <= 0)
        return;

    union { uint64_t align; uint8_t b[NLRT_MSG_MAX]; } msg;
    for (int i = 0; i < n; i++) {
        const nlrt_link_t *e = NULL;
        for (int k = 0; k < nlinks; k++)
            if (links[k].index == events[i].index) {
                e = &links[k];
                break;
            }
        if (!e)
            continue;               /* the netif went away; nothing to publish */
        size_t total = nlrt_build_linkmsg(msg.b, e, RTM_NEWLINK, 0,
                                          nlrt_next_seq());
        if (total == 0)
            continue;
        (void)nlrt_broadcast(msg.b, total, RTNLGRP_LINK);
    }
}

/*
 * An interface's IPv4 address changed.  `addr` / `mask` are the values that
 * were just applied (or just removed), not a re-read: after a delete the slot
 * is empty, and a notification carrying 0.0.0.0 would tell every listener to
 * forget an address without ever naming it.
 *
 * An all-zero address is published as RTM_DELADDR rather than RTM_NEWADDR.
 * Emitting RTM_NEWADDR with 0.0.0.0 would be a message no listener can act on:
 * "you now have address 0.0.0.0" is not a state any interface is in.
 */
void net_netlink_addr_notify(unsigned ifindex, const uint8_t addr[4],
                             const uint8_t mask[4])
{
    if (!addr)
        return;
    nlrt_link_t links[NLRT_MAX_LINKS];
    int nlinks = nlrt_snapshot(links, NLRT_MAX_LINKS);
    const nlrt_link_t *e = NULL;
    for (int k = 0; k < nlinks; k++)
        if (links[k].index == ifindex) {
            e = &links[k];
            break;
        }
    if (!e)
        return;

    int deleted = nlrt_ip4_isany(addr);
    union { uint64_t align; uint8_t b[NLRT_MSG_MAX]; } msg;
    size_t total = nlrt_build_addrmsg(msg.b, e->index, addr, mask, e->name,
                                      e->loopback,
                                      deleted ? RTM_DELADDR : RTM_NEWADDR,
                                      0, nlrt_next_seq());
    if (total == 0)
        return;
    (void)nlrt_broadcast(msg.b, total, RTNLGRP_IPV4_IFADDR);
}
