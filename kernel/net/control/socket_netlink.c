#include "proc/userns.h"
#include "net/socket/socket_internal.h"
#include "net/tun.h"

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


#include "net/socket_netlink_state.h"

/* The rtnetlink flag bits the snapshot primitive reports; the Rust module
 * carries its own copies of the values it compares against. */
#define NLRT_IF_FLAGS_UP       0x1
#define NLRT_IF_FLAGS_LOOPBACK 0x8
#define NLRT_IF_FLAGS_RUNNING  0x40

/*
 * The AF_NETLINK state machines are Rust (kernel/net/control/socket_netlink.rs): the
 * bind transaction, sock_diag, the kobject-uevent emitter and sender, and the
 * rtnetlink request/response and multicast paths.  What is left here is the
 * primitive set they call, the snapshot the rtnetlink replies are built from,
 * and the wrappers that keep the C names the rest of the tree uses.
 *
 * The per-bucket table walks live in Rust because their filter is a decision;
 * only reaching the slot and taking its locks is C's.
 */

/* The Rust `NlrtLink`, filled by the snapshot primitive.  Same layout as the
 * Rust struct; the offsets the Rust side reads are pinned below. */
typedef struct {
    char     name[16];
    uint32_t index;
    uint32_t mtu;
    uint32_t flags;
    uint8_t  mac[6];
    uint8_t  loopback;
    uint8_t  has_ip;
    uint8_t  has_gw;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
} nl_link_out_t;

_Static_assert(sizeof(nl_link_out_t) == 52 &&
               offsetof(nl_link_out_t, index) == 16 &&
               offsetof(nl_link_out_t, ip) == 37 &&
               offsetof(nl_link_out_t, mask) == 41 &&
               offsetof(nl_link_out_t, gw) == 45,
               "kernel/net/control/socket_netlink.rs declares NlrtLink at this layout");

void a20_net_netlink_view_init(void *sp, net_netlink_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    out->s = s;
    out->net_ns = (void **)&s->net_ns;
    out->domain = &s->domain;
    out->sock_type = &s->type;
    out->protocol = &s->protocol;
    out->bound = &s->bound;
    out->connected = &s->connected;
    out->listening = &s->listening;
    out->local = s->local;
    out->local_len = &s->local_len;
    out->peer_addr = s->peer_addr;
    out->peer_len = &s->peer_len;
}

void a20_net_netlink_lock(void *s, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)s);
}

void a20_net_netlink_unlock(void *s, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)s, flags);
}

int a20_net_netlink_buckets(void)
{
    return NET_SOCK_BUCKETS;
}

int a20_net_netlink_slots_per_bucket(void)
{
    return NET_SOCK_SLOTS_PER_BUCKET;
}

void a20_net_netlink_bucket_lock(int bucket, uint64_t *flags)
{
    *flags = net_bucket_lock(bucket);
}

void a20_net_netlink_bucket_unlock(int bucket, uint64_t flags)
{
    net_bucket_unlock(bucket, flags);
}

void *a20_net_netlink_slot_socket(int bucket, int i)
{
    return g_sockets[(bucket << NET_SOCK_BUCKET_SHIFT) + i];
}

int a20_net_netlink_is_live(void *s)
{
    return net_socket_is_live((const net_socket_t *)s) ? 1 : 0;
}

int a20_net_netlink_enqueue(void *s, const uint8_t *buf, size_t len,
                            const void *src, size_t srclen)
{
    return net_enqueue_msg_locked((net_socket_t *)s, buf, len, src, srclen);
}

uint32_t a20_net_netlink_rx_bytes(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    uint64_t total = 0;
    for (const net_msg_t *m = s->rx_head; m; m = m->next)
        total += m->len - m->off;
    return total > 0xffffffffULL ? 0xffffffffU : (uint32_t)total;
}

uint32_t a20_net_netlink_local_pid(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    if (s->local_len < sizeof(net_sockaddr_nl_t))
        return 0;
    return ((const net_sockaddr_nl_t *)s->local)->nl_pid;
}

uint32_t a20_net_netlink_local_groups(void *sp)
{
    net_socket_t *s = (net_socket_t *)sp;
    if (s->local_len < sizeof(net_sockaddr_nl_t))
        return 0;
    return ((const net_sockaddr_nl_t *)s->local)->nl_groups;
}

uint32_t a20_net_netlink_host_pid(void)
{
    task_t *cur = proc_current();
    return cur ? (uint32_t)cur->pid : 0;
}

int a20_net_netlink_ns_initial(void *ns)
{
    return resource_ns_is_initial((const resource_namespace_t *)ns) ? 1 : 0;
}

void *a20_net_netlink_current_ns(void)
{
    return resource_ns_task_get(NULL, PROCNS_NET);
}

int a20_net_netlink_capable(void *ns)
{
    return userns_capable(proc_current(),
                          ((resource_namespace_t *)ns)->owner,
                          CAP_NET_ADMIN) ? 1 : 0;
}

void a20_net_netlink_class_emit_all(const char *action)
{
    class_device_emit_uevents(action);
}

void *a20_net_netlink_class_get(const char *name)
{
    return class_device_get_by_name(name);
}

const char *a20_net_netlink_class_subsystem(void *cdev)
{
    return class_device_subsystem(((class_device_t *)cdev)->class_type);
}

uint64_t a20_net_netlink_class_devt(void *cdev)
{
    return ((class_device_t *)cdev)->devt;
}

void a20_net_netlink_class_put(void *cdev)
{
    class_device_put((class_device_t *)cdev);
}

/* Copy the interface list out of lwIP.  The lock contract forbids holding
 * g_lwip_lock together with a net lock, so the snapshot is taken and the lwIP
 * lock dropped before any reply is enqueued. */
int a20_net_netlink_snapshot(void *nsp, void *outp, int max)
{
    resource_namespace_t *ns = (resource_namespace_t *)nsp;
    nl_link_out_t *out = (nl_link_out_t *)outp;
    if (!resource_ns_is_initial(ns)) {
        if (max < 1) return 0;
        nl_link_out_t *e = &out[0];
        memset(e, 0, sizeof(*e));
        strcpy(e->name, "lo"); e->index = 1; e->loopback = 1;
        e->mtu = __atomic_load_n(&ns->loopback_mtu, __ATOMIC_ACQUIRE);
        e->flags = NLRT_IF_FLAGS_LOOPBACK;
        if (__atomic_load_n(&ns->loopback_up, __ATOMIC_ACQUIRE))
            e->flags |= NLRT_IF_FLAGS_UP | NLRT_IF_FLAGS_RUNNING;
        e->has_ip = 1; e->ip[0] = 127; e->ip[3] = 1; e->mask[0] = 255;
        return 1;
    }
    uint64_t lf = a20_lwip_lock();
    int n = 0;
    for (struct netif *ni = netif_list; ni && n < max; ni = ni->next) {
        nl_link_out_t *e = &out[n++];
        memset(e, 0, sizeof(*e));
        /* A TUN netif carries a user-chosen name ("tailscale0"), which does
         * not fit lwIP's 2-char + num scheme; ask the service for it. */
        char tun_name[16];
        int is_tun = a20_tun_netif_name(ni, tun_name, sizeof(tun_name));
        if (is_tun)
            snprintf(e->name, sizeof(e->name), "%s", tun_name);
        else
            snprintf(e->name, sizeof(e->name), "%c%c%u",
                     ni->name[0], ni->name[1], ni->num);
        e->index = (uint32_t)netif_get_index(ni);
        e->mtu = ni->mtu;
        e->flags = NLRT_IF_FLAGS_LOOPBACK;
        if (netif_is_up(ni))
            e->flags |= NLRT_IF_FLAGS_UP;
        if (netif_is_link_up(ni))
            e->flags |= NLRT_IF_FLAGS_RUNNING;
        if (ni->flags & NETIF_FLAG_ETHERNET) {
            e->loopback = 0, e->flags &= ~(uint32_t)NLRT_IF_FLAGS_LOOPBACK;
        } else if (is_tun) {
            /* TUN is an L3 point-to-point interface, never the loopback one:
             * leaving IFF_LOOPBACK set makes net.Interfaces() (and therefore
             * tailscale's netmon) treat the tunnel as lo. */
            e->loopback = 0, e->flags &= ~(uint32_t)NLRT_IF_FLAGS_LOOPBACK;
        } else {
            e->loopback = 1;
        }
        memcpy(e->mac, ni->hwaddr, 6);
        const ip4_addr_t *ip = netif_ip4_addr(ni);
        const ip4_addr_t *mask = netif_ip4_netmask(ni);
        const ip4_addr_t *gw = netif_ip4_gw(ni);
        if (ip && !ip4_addr_isany_val(*ip)) {
            e->has_ip = 1;
            e->ip[0] = ip4_addr1(ip); e->ip[1] = ip4_addr2(ip);
            e->ip[2] = ip4_addr3(ip); e->ip[3] = ip4_addr4(ip);
        }
        if (mask) {
            e->mask[0] = ip4_addr1(mask); e->mask[1] = ip4_addr2(mask);
            e->mask[2] = ip4_addr3(mask); e->mask[3] = ip4_addr4(mask);
        }
        if (gw && !ip4_addr_isany_val(*gw)) {
            e->has_gw = 1;
            e->gw[0] = ip4_addr1(gw); e->gw[1] = ip4_addr2(gw);
            e->gw[2] = ip4_addr3(gw); e->gw[3] = ip4_addr4(gw);
        }
    }
    a20_lwip_unlock(lf);
    return n;
}

uint32_t a20_net_netlink_event_index(const void *eventsp, int i)
{
    const nlrt_link_event_t *events = (const nlrt_link_event_t *)eventsp;
    return events[i].index;
}

int a20_net_netlink_if_set_mtu(uint32_t ifindex, uint16_t mtu)
{
    return a20_lwip_if_set_mtu((unsigned)ifindex, mtu);
}

int a20_net_netlink_if_set_flags(uint32_t ifindex, uint32_t value, uint32_t mask)
{
    return a20_lwip_if_set_flags((unsigned)ifindex, value, mask);
}

int a20_net_netlink_if_get_addr(uint32_t ifindex, uint8_t *ip, uint8_t *mask,
                                uint8_t *gw)
{
    return a20_lwip_if_get_addr((unsigned)ifindex, ip, mask, gw);
}

int a20_net_netlink_if_set_addr(uint32_t ifindex, const uint8_t *ip,
                                const uint8_t *mask, const uint8_t *gw)
{
    return a20_lwip_if_set_addr((unsigned)ifindex, ip, mask, gw);
}

int a20_net_netlink_if_set_addr6(uint32_t ifindex, const uint8_t *addr,
                                 uint8_t prefixlen)
{
    return a20_lwip_if_set_addr6((unsigned)ifindex, addr, prefixlen);
}

int a20_net_netlink_if_del_addr6(uint32_t ifindex, const uint8_t *addr)
{
    return a20_lwip_if_del_addr6((unsigned)ifindex, addr);
}

uint32_t a20_net_netlink_loopback_mtu(void *ns, uint32_t value, int set)
{
    resource_namespace_t *r = (resource_namespace_t *)ns;
    if (set)
        __atomic_store_n(&r->loopback_mtu, value, __ATOMIC_RELEASE);
    return __atomic_load_n(&r->loopback_mtu, __ATOMIC_ACQUIRE);
}

uint32_t a20_net_netlink_loopback_up(void *ns, uint32_t value, int set)
{
    resource_namespace_t *r = (resource_namespace_t *)ns;
    if (set)
        __atomic_store_n(&r->loopback_up, value, __ATOMIC_RELEASE);
    return __atomic_load_n(&r->loopback_up, __ATOMIC_ACQUIRE);
}

void a20_net_netlink_klog_uevent(const char *action, const char *subsystem,
                                 const char *name, uint32_t major,
                                 uint32_t minor, int delivered)
{
    klog(KLOG_INFO, "[UEVENT] %s %s/%s devt=%u:%u delivered=%d\n",
         action, subsystem, name, major, minor, delivered);
}

void a20_net_netlink_klog_drop(uint32_t group, int dropped, int delivered)
{
    klog(KLOG_WARN,
         "[RTNETLINK] multicast group 0x%x dropped for %d listener(s), delivered=%d\n",
         (unsigned)group, dropped, delivered);
}

int net_netlink_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    return a20_net_netlink_bind(s, addr, addrlen);
}

int net_netlink_diag_request(net_socket_t *requester, const void *buf,
                             size_t len, const void *addr, size_t addrlen)
{
    return a20_net_netlink_diag_request(requester, buf, len, addr, addrlen);
}

void netlink_uevent_emit(const char *action, const char *subsystem,
                         const char *name, uint64_t devt)
{
    a20_net_netlink_uevent_emit(action, subsystem, name, devt);
}

int net_netlink_uevent_send(net_socket_t *requester, const void *buf,
                            size_t len, const void *addr, size_t addrlen)
{
    return a20_net_netlink_uevent_send(requester, buf, len, addr, addrlen);
}

int net_netlink_route_request(net_socket_t *requester, const void *buf,
                              size_t len, const void *addr, size_t addrlen)
{
    return a20_net_netlink_route_request(requester, buf, len, addr, addrlen);
}

void net_netlink_link_notify(const nlrt_link_event_t *events, int n)
{
    a20_net_netlink_link_notify(events, n);
}

void net_netlink_addr_notify(unsigned ifindex, const uint8_t addr[4],
                             const uint8_t mask[4])
{
    a20_net_netlink_addr_notify(ifindex, addr, mask);
}

void net_netlink_addr6_notify(unsigned ifindex, const uint8_t addr[16],
                              uint8_t prefixlen, uint16_t msg_type)
{
    a20_net_netlink_addr6_notify(ifindex, addr, prefixlen, msg_type);
}

