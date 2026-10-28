#include "net/socket/socket_internal.h"
#include "net/tun.h"
#include "net/socket_file_state.h"
#include "proc/userns.h"
#include "ipc/ipc.h"
#include "net/lwip_stack.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/string.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/readiness.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

static int net_vfile_read(vfile_t *vf, char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    /* read(2) on a socket is a receive with no flags, no address and no
     * metadata, which is exactly what the Rust receive transaction already
     * is: the algorithm dispatch, the channel path and the queue path with
     * its coalescing, refusal ordering, park and accounting all live there.
     * Keeping a second copy of that state machine here only risked the two
     * drifting apart. */
    return net_recvfrom_socket_meta(s, buf, count, 0, NULL, NULL, NULL);
}

static int net_vfile_write(vfile_t *vf, const char *buf, size_t count) {
    net_socket_t *s = vf ? (net_socket_t *)vf->priv : NULL;
    if (!s)
        return -ENOTSOCK;
    if (count > NET_MAX_PAYLOAD && s->type != SOCK_STREAM)
        return -EMSGSIZE;
    if (s->domain == AF_ALG)
        return net_alg_socket_send(s, buf, count);
    if ((s->domain == AF_INET || s->domain == AF_INET6) &&
        (s->udp || s->raw || s->tcp))
        return net_inet_sendto(s, buf, count, 0, NULL, 0);
    /* Same rule and same reason as net_sendto_sock(): a stream socket in an
     * inet family with no pcb is a connection lwIP tore down, not a
     * non-socket, so it owes the caller EPIPE rather than ENOTSOCK. */
    if (s->type == SOCK_STREAM && !s->local_tcp && !s->tcp &&
        (s->domain == AF_INET || s->domain == AF_INET6))
        return s->ever_connected ? -EPIPE
                                  : net_notconn(NET_NOTCONN_VFS_WRITE_NO_PCB);
    if (s->domain == AF_UNIX)
        return net_unix_socket_sendto(s, buf, count, NULL, 0);

    /* Destination first, outside any lock: it comes from a whole-table scan that
     * walks the registry a bucket at a time, and the scan hands back a
     * reference, which is what lets this function then take s's and dst's own
     * locks without nesting a third one. */
    uint8_t src_local[NET_SOCKADDR_MAX];
    size_t src_local_len = 0;
    uint8_t peer_addr[NET_SOCKADDR_MAX];
    size_t peer_len = 0;
    bool connected = false;
    bool nonblock = false;
    uint64_t send_timeout = 0;
    net_socket_t *dst = NULL;
    {
        uint64_t sf = net_sock_lock(s);
        if (!net_socket_is_live(s)) {
            net_sock_unlock(s, sf);
            return -ENOTSOCK;
        }
        if (s->closed || s->shut_wr) {
            net_sock_unlock(s, sf);
            return -EPIPE;
        }
        if (!s->bound && (s->domain == AF_INET || s->domain == AF_INET6))
            net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
        src_local_len = s->local_len;
        memcpy(src_local, s->local, src_local_len);
        /* The peer pointer is sampled under s's lock, which is what keeps it
         * from being freed between the test and the net_socket_ref() below. */
        net_socket_t *peer = s->peer;
        if (peer && s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET &&
            !net_socket_is_live(peer))
            s->peer = NULL;
        dst = s->peer ? net_socket_ref(s->peer) : NULL;
        /* Everything below runs with no lock on s, so s's own answers are
         * copied out here rather than re-read. */
        connected = s->connected;
        nonblock = s->nonblock;
        send_timeout = s->send_timeout_ticks;
        peer_len = s->peer_len;
        if (peer_len)
            memcpy(peer_addr, s->peer_addr, peer_len);
        net_sock_unlock(s, sf);
    }
    if (!dst && connected)
        dst = net_find_bound_socket(s->net_ns, s->domain, s->type, peer_addr, peer_len);
    if (!dst)
        return connected ? -ECONNREFUSED : -EDESTADDRREQ;

    if (count <= NET_MAX_PAYLOAD) {
        int r;
        net_sock_pair_t pair = net_sock_lock2(s, dst);
        if (!net_socket_is_live(dst)) {
            net_sock_unlock2(pair);
            net_socket_free(dst);
            return connected ? -ECONNREFUSED : -EDESTADDRREQ;
        }
        if (dst->rx_count >= NET_MAX_QUEUE && !nonblock) {
            net_sock_unlock2(pair);
            r = net_enqueue_msg_blocking(s, dst, buf, count, src_local,
                                         src_local_len, nonblock, send_timeout);
            net_socket_free(dst);
            return r;
        }
        r = net_enqueue_msg_locked(dst, buf, count, src_local, src_local_len);
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        if (r >= 0)
            (void)wait_queue_collect_one(
                &dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q);
        net_sock_unlock2(pair);
        (void)proc_wake_q_flush(&wake_q);
        net_socket_free(dst);
        return r;
    }

    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > NET_MAX_PAYLOAD)
            chunk = NET_MAX_PAYLOAD;
        int r = net_enqueue_msg_blocking(s, dst, buf + total, chunk,
                                         src_local, src_local_len,
                                         nonblock, send_timeout);
        if (r < 0) {
            net_socket_free(dst);
            return total ? (int)total : r;
        }
        total += (size_t)r;
    }
    net_socket_free(dst);
    return (int)total;
}

static long net_vfile_lseek(vfile_t *vf, long offset, int whence) {
    (void)vf; (void)offset; (void)whence;
    return -ESPIPE;
}


static size_t net_vfile_poll_sources(vfile_t *vf, short events,
                                     readiness_source_t *sources, size_t max)
{
    net_socket_t *s = vf ? vf->priv : NULL;
    if (!s || !sources)
        return 0;
    size_t count = 0;
    if (((events & POLLIN) || !(events & (POLLIN | POLLOUT))) && count < max) {
        wait_queue_t *queue = s->listening ? &s->accept_waitq : &s->read_waitq;
        sources[count++] = (readiness_source_t){ queue, 0, 0 };
        if (s->ch_ep && count < max)
            sources[count++] = (readiness_source_t){
                &s->ch_ep->waiters, A20_CH_WAIT_RECV, 0
            };
    }
    if ((events & POLLOUT) && count < max) {
        sources[count++] = (readiness_source_t){ &s->write_waitq, 0, 0 };
        if (s->ch_ep && count < max)
            sources[count++] = (readiness_source_t){
                &s->ch_ep->waiters, A20_CH_WAIT_SEND, 0
            };
    }
    return count;
}

/*
 * Interface ioctls (SIOCGIF*).  Standard network tooling -- udhcpc,
 * ifconfig, busybox `ip` -- configures an interface through these; with no
 * ioctl op at all the very first SIOCGIFHWADDR returned ENOTTY and the
 * interface could never be brought up.  Values match Linux/musl
 * <sys/ioctl.h>.  A struct ifreq is 16-byte name + a 16-byte union; the
 * address ioctls return a `struct sockaddr` (family + packed bytes).
 */
#define A20_SIOCGIFNAME     0x8910
#define A20_SIOCGIFCONF     0x8912
#define A20_SIOCGIFINDEX    0x8933
#define A20_SIOCGIFFLAGS    0x8913
#define A20_SIOCSIFFLAGS    0x8914
#define A20_SIOCGIFADDR     0x8915
#define A20_SIOCSIFADDR     0x8916
#define A20_SIOCGIFDSTADDR  0x8917
#define A20_SIOCSIFDSTADDR  0x8918
#define A20_SIOCGIFBRDADDR  0x8919
#define A20_SIOCSIFBRDADDR  0x891a
#define A20_SIOCGIFNETMASK  0x891b
#define A20_SIOCSIFNETMASK  0x891c
#define A20_SIOCGIFMTU      0x8921
#define A20_SIOCSIFMTU      0x8922
#define A20_SIOCGIFHWADDR   0x8927

#define A20_IFNAMSIZ        16
#define A20_IFF_UP          0x1
#define A20_IFF_BROADCAST   0x2
#define A20_IFF_LOOPBACK    0x8
#define A20_IFF_RUNNING     0x40
#define A20_ARPHRD_ETHER    1
#define A20_ARPHRD_LOOPBACK 772
#define A20_AF_UNSPEC       0
#define A20_AF_INET         2

/* Linux's `struct ifmap`, which is what sizes the ifreq union.  It is included
 * because its width depends on `unsigned long`, exactly as upstream's does, so
 * the resulting ifreq size comes out right on both LP64 and ILP32.  Omitting it
 * leaves the union 16 bytes wide, the struct 8 bytes shorter than every real
 * libc's, and a caller walking the SIOCGIFCONF buffer in whole `struct ifreq`
 * strides desynchronises after the first entry. */
struct a20_ifmap {
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char  irq;
    unsigned char  dma;
    unsigned short port;
};

struct a20_ifreq {
    char ifr_name[A20_IFNAMSIZ];
    union {
        uint8_t raw[16];
        struct { uint16_t sa_family; uint16_t sa_port; uint32_t sa_addr; uint8_t sa_zero[8]; } addr;
        struct a20_ifmap map;
        char slave[A20_IFNAMSIZ];
        char newname[A20_IFNAMSIZ];
        void *data;
        short flags;
        int ivalue;
    } ifr_ifru;
};

/* User space indexes the SIOCGIFCONF buffer with its own libc definition of
 * this struct, so the size must match musl/glibc exactly.  It is 40 bytes on
 * LP64 and 32 on ILP32, so one hardcoded constant would be wrong on one of
 * them and both are asserted instead.  A flat `sizeof == 32` assertion is what
 * let the original mismatch ship: it passed while every real caller was
 * reading garbage. */
_Static_assert(sizeof(void *) == 8 ? sizeof(struct a20_ifreq) == 40
                                   : sizeof(struct a20_ifreq) == 32,
               "struct ifreq wire layout must match Linux/musl");
_Static_assert(offsetof(struct a20_ifreq, ifr_ifru) == 16, "ifreq union offset");

/* Linux `struct ifconf`: ifc_len is in/out.  With a NULL ifc_buf the caller is
 * asking how large a full listing would be, which is why the size query below
 * is computed exactly rather than estimated. */
struct a20_ifconf {
    int   ifc_len;
    void *ifc_buf;
};

/* Fill one SIOCGIFCONF row: interface name plus its IPv4 address.
 *
 * A netif's identity here is (name[0], name[1], num), and lwIP's netif_find
 * parses the number out of name[2] -- it rejects a name with no digit there.
 * The reported name is therefore composed rather than copied, which is also
 * the spelling /proc/net/status already uses and the only one
 * net_ifreq_lookup can resolve. */
static void net_ifreq_fill(struct a20_ifreq *ifr, const struct netif *nif)
{
    memset(ifr, 0, sizeof(*ifr));
    /* A TUN netif carries a real interface name ("tailscale0"); the lwIP
     * two-char plus number spelling would hide it from if_nametoindex(). */
    if (!a20_tun_netif_name((struct netif *)nif, ifr->ifr_name,
                            sizeof(ifr->ifr_name)))
        snprintf(ifr->ifr_name, sizeof(ifr->ifr_name), "%c%c%u",
                 nif->name[0], nif->name[1], nif->num);

    /* The row carries a `struct sockaddr_in`.  sin_family is a raw host-order
     * constant -- Linux code writes `sin_family = AF_INET` and every libc
     * header documents it that way, so it must not be byteswapped.  sin_addr
     * is the part that was wrong: it lives at offset 4, after sin_family and
     * sin_port, and writing it at offset 2 lands in sin_port, which is how
     * 10.0.2.15 was reported as 2.15.0.0. */
    ifr->ifr_ifru.addr.sa_family = A20_AF_INET;
    ifr->ifr_ifru.addr.sa_port = 0;
    memcpy(&ifr->ifr_ifru.addr.sa_addr, &nif->ip_addr, sizeof(nif->ip_addr));
}

/* SIOCGIFCONF: enumerate the configured interfaces.  getifaddrs(), ifconfig
 * and busybox `ip` are all built on it, and the per-interface getters above
 * are unreachable without it. */
static int net_ifreq_getconf(void *uarg)
{
    struct a20_ifconf ifc;
    if (copy_from_user(&ifc, uarg, sizeof(ifc)) < 0)
        return -EFAULT;
    int needed = 0;
    for (struct netif *n = netif_list; n; n = n->next)
        needed += (int)sizeof(struct a20_ifreq);

    if (!ifc.ifc_buf) {
        ifc.ifc_len = needed;
        return copy_to_user(uarg, &ifc, sizeof(ifc)) < 0 ? -EFAULT : 0;
    }

    /* The row buffer is output only, including its address-family fields. */
    int limit = ifc.ifc_len;
    int used = 0;
    for (struct netif *n = netif_list; n; n = n->next) {
        if (used + (int)sizeof(struct a20_ifreq) > limit)
            break;
        struct a20_ifreq ifr;
        net_ifreq_fill(&ifr, n);
        if (copy_to_user((uint8_t *)ifc.ifc_buf + used, &ifr, sizeof(ifr)) < 0)
            return -EFAULT;
        used += (int)sizeof(ifr);
    }
    ifc.ifc_len = used;
    return copy_to_user(uarg, &ifc, sizeof(ifc)) < 0 ? -EFAULT : 0;
}

static struct netif *net_ifreq_lookup(const char *name)
{
    if (!name[0])
        return netif_default;
    struct netif *n = netif_find(name);
    if (n)
        return n;
    /* TUN names are not expressible in lwIP's name scheme, so ask the service
     * before falling through to the default-interface aliases. */
    n = a20_tun_netif_by_name(name);
    if (n)
        return n;
    /* A20OS names its lwIP netif "en0" while the device node users see is
     * "net0"/"eth0"; accept those spellings as the default interface. */
    if (!strncmp(name, "en", 2) || !strncmp(name, "eth", 3) ||
        !strncmp(name, "net", 3))
        return netif_default;
    return NULL;
}

static int net_ifreq_put_addr(void *uarg, uint16_t family,
                              const void *bytes, size_t n)
{
    uint8_t out[16];
    memset(out, 0, sizeof(out));
    /* sockaddr_in, not a two-byte-family header: sin_family, sin_port, then
     * sin_addr at offset 4.  Writing the address at offset 2 puts it in
     * sin_port and shifts every address by two bytes, so 10.0.2.15 was read
     * back as 2.15.0.0. */
    out[0] = (uint8_t)family;
    out[1] = (uint8_t)(family >> 8);
    if (n > sizeof(out) - 4)
        n = sizeof(out) - 4;
    memcpy(out + 4, bytes, n);
    return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, out, sizeof(out)) < 0
           ? -EFAULT : 0;
}

static int net_vfile_ifreq_ioctl(void *uarg, unsigned long req)
{
    struct a20_ifreq ifr;
    if (copy_from_user(&ifr, uarg, sizeof(ifr)) < 0)
        return -EFAULT;

    struct netif *nif = net_ifreq_lookup(ifr.ifr_name);
    if (!nif)
        return -ENODEV;

    switch (req) {
    case A20_SIOCGIFHWADDR:
        return net_ifreq_put_addr(uarg, (nif->flags & NETIF_FLAG_ETHERNET) ?
                                  A20_ARPHRD_ETHER : A20_ARPHRD_LOOPBACK,
                                  nif->hwaddr, nif->hwaddr_len);
    case A20_SIOCGIFADDR:
        return net_ifreq_put_addr(uarg, A20_AF_INET, &nif->ip_addr, 4);
    case A20_SIOCGIFNETMASK:
        return net_ifreq_put_addr(uarg, A20_AF_INET, &nif->netmask, 4);
    case A20_SIOCGIFDSTADDR:
    case A20_SIOCGIFBRDADDR: {
        uint32_t ip = 0, mask = 0, brd;
        memcpy(&ip, &nif->ip_addr, 4);
        memcpy(&mask, &nif->netmask, 4);
        brd = ip | ~mask;
        return net_ifreq_put_addr(uarg, A20_AF_INET, &brd, 4);
    }
    case A20_SIOCGIFMTU: {
        int mtu = (int)nif->mtu;
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &mtu, sizeof(mtu)) < 0
               ? -EFAULT : 0;
    }
    case A20_SIOCSIFMTU: {
        /*
         * Routed through a20_lwip_if_set_mtu(), not a bare nif->mtu write, and
         * the reason is the same one RTM_NEWLINK goes through it for: the
         * RFC 791 floor and the profile's frame-size ceiling are enforced
         * there and nowhere else.  Setting nif->mtu directly would be the one
         * way to configure an MTU the receive path then silently truncates at
         * its scratch-buffer clamp and the transmit path refuses with ERR_BUF.
         *
         * This case was missing -- SIOCSIFMTU was dispatched here by
         * net_vfile_ioctl() but had no arm in the switch, so every caller got
         * -ENOTTY and the ceiling a20_lwip_if_set_mtu() enforces was reachable
         * only from netlink.  That is why the smoke for the ceiling had to go
         * through RTM_NEWLINK until now; it now covers both entry points.
         *
         * The range check is here rather than in the callee because that takes
         * uint16_t: an ifreq's ifr_mtu is an int, and 0x10000 would truncate
         * back to 0 and be refused as a below-minimum MTU rather than as the
         * out-of-range value it is.
         */
        int mtu;
        if (copy_from_user(&mtu, (uint8_t *)uarg + A20_IFNAMSIZ, sizeof(mtu)) < 0)
            return -EFAULT;
        if (mtu < 0 || mtu > 0xffff)
            return -EINVAL;
        return a20_lwip_if_set_mtu(netif_get_index(nif), (uint16_t)mtu);
    }
    case A20_SIOCGIFINDEX: {
        /* Not netif_name_to_index(): netif->name is a two-character
         * abbreviation, not a NUL-terminated name, so that lookup compared
         * against garbage and every interface answered 0.  This is also the
         * numbering the netlink path and a20_lwip_netif_by_index() use. */
        int idx = (int)netif_get_index(nif);
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &idx, sizeof(idx)) < 0
               ? -EFAULT : 0;
    }
    case A20_SIOCGIFNAME:
        return 0;
    case A20_SIOCGIFFLAGS: {
        /* Only the 127.0.0.1 netif is a loopback.  Reporting every
         * non-Ethernet interface as IFF_LOOPBACK made a TUN look like "lo"
         * (ifconfig printed "Link encap:Local Loopback"), and netmon users
         * discard loopback links when choosing a default route. */
        short fl = 0;
        if (nif->flags & NETIF_FLAG_ETHERNET)
            fl |= A20_IFF_BROADCAST;
        if (netif_ip4_addr(nif)->addr == 0x0100007fU)
            fl |= A20_IFF_LOOPBACK;
        if (nif->flags & NETIF_FLAG_UP)
            fl |= A20_IFF_UP;
        if (nif->flags & NETIF_FLAG_LINK_UP)
            fl |= A20_IFF_RUNNING;
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &fl, sizeof(fl)) < 0
               ? -EFAULT : 0;
    }
    case A20_SIOCSIFFLAGS: {
        short fl = 0;
        if (copy_from_user(&fl, (uint8_t *)uarg + A20_IFNAMSIZ, sizeof(fl)) < 0)
            return -EFAULT;
        if (fl & A20_IFF_UP)
            netif_set_up(nif);
        else
            netif_set_down(nif);
        return 0;
    }
    case A20_SIOCSIFADDR: {
        uint8_t a[16];
        ip4_addr_t ip;
        if (copy_from_user(a, (uint8_t *)uarg + A20_IFNAMSIZ, sizeof(a)) < 0)
            return -EFAULT;
        memcpy(&ip, a + 2, 4);
        netif_set_ipaddr(nif, &ip);
        return 0;
    }
    case A20_SIOCSIFNETMASK: {
        uint8_t a[16];
        ip4_addr_t nm;
        if (copy_from_user(a, (uint8_t *)uarg + A20_IFNAMSIZ, sizeof(a)) < 0)
            return -EFAULT;
        memcpy(&nm, a + 2, 4);
        netif_set_netmask(nif, &nm);
        return 0;
    }
    }
    return -ENOTTY;
}

/* A newly created network namespace owns an initially-down loopback and no
 * physical NIC. The fd's namespace is used even after its owner calls setns. */
static int net_namespace_ioctl(resource_namespace_t *ns, unsigned long req, void *uarg)
{
    if (req == A20_SIOCGIFCONF) {
        struct a20_ifconf ifc;
        if (copy_from_user(&ifc, uarg, sizeof(ifc)) < 0) return -EFAULT;
        if (!ifc.ifc_buf) ifc.ifc_len = sizeof(struct a20_ifreq);
        else if (ifc.ifc_len >= (int)sizeof(struct a20_ifreq)) {
            struct a20_ifreq ifr = {0};
            strcpy(ifr.ifr_name, "lo");
            ifr.ifr_ifru.addr.sa_family = A20_AF_INET;
            ifr.ifr_ifru.addr.sa_addr = 0x0100007f;
            if (copy_to_user(ifc.ifc_buf, &ifr, sizeof(ifr)) < 0) return -EFAULT;
            ifc.ifc_len = sizeof(ifr);
        } else ifc.ifc_len = 0;
        return copy_to_user(uarg, &ifc, sizeof(ifc)) < 0 ? -EFAULT : 0;
    }
    struct a20_ifreq ifr;
    if (copy_from_user(&ifr, uarg, sizeof(ifr)) < 0) return -EFAULT;
    ifr.ifr_name[A20_IFNAMSIZ - 1] = 0;
    if (req == A20_SIOCGIFNAME) {
        int index; memcpy(&index, ifr.ifr_ifru.raw, sizeof(index));
        if (index != 1) return -ENODEV;
        strcpy(ifr.ifr_name, "lo");
        return copy_to_user(uarg, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    }
    if (strcmp(ifr.ifr_name, "lo")) return -ENODEV;
    uint32_t value = 0;
    switch (req) {
    case A20_SIOCGIFINDEX: value = 1; break;
    case A20_SIOCGIFMTU: value = __atomic_load_n(&ns->loopback_mtu, __ATOMIC_ACQUIRE); break;
    case A20_SIOCGIFFLAGS:
        value = 8 | (__atomic_load_n(&ns->loopback_up, __ATOMIC_ACQUIRE) ? A20_IFF_UP | A20_IFF_RUNNING : 0);
        return copy_to_user((char *)uarg + A20_IFNAMSIZ, &value, sizeof(short)) < 0 ? -EFAULT : 0;
    case A20_SIOCGIFADDR: value = 0x0100007f; return net_ifreq_put_addr(uarg, A20_AF_INET, &value, 4);
    case A20_SIOCGIFNETMASK: value = 0x000000ff; return net_ifreq_put_addr(uarg, A20_AF_INET, &value, 4);
    case A20_SIOCGIFHWADDR: { uint8_t zero[6] = {0}; return net_ifreq_put_addr(uarg, 772, zero, 6); }
    case A20_SIOCSIFFLAGS:
    case A20_SIOCSIFMTU:
        if (!userns_capable(proc_current(), ns->owner, CAP_NET_ADMIN)) return -EPERM;
        memcpy(&value, ifr.ifr_ifru.raw, req == A20_SIOCSIFFLAGS ? sizeof(short) : sizeof(int));
        if (req == A20_SIOCSIFFLAGS) __atomic_store_n(&ns->loopback_up, !!(value & A20_IFF_UP), __ATOMIC_RELEASE);
        else {
            if (value < 68 || value > 65536) return -EINVAL;
            __atomic_store_n(&ns->loopback_mtu, value, __ATOMIC_RELEASE);
        }
        return 0;
    default: return -EOPNOTSUPP;
    }
    return copy_to_user((char *)uarg + A20_IFNAMSIZ, &value, sizeof(value)) < 0 ? -EFAULT : 0;
}

static int net_vfile_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    net_socket_t *s = vf ? vf->priv : NULL;
    if (s && !resource_ns_is_initial(s->net_ns))
        return net_namespace_ioctl(s->net_ns, req, arg);
    if (req == A20_SIOCGIFCONF)
        return net_ifreq_getconf(arg);
    switch (req) {
    case A20_SIOCGIFNAME: case A20_SIOCGIFFLAGS:
    case A20_SIOCSIFFLAGS: case A20_SIOCGIFADDR: case A20_SIOCSIFADDR:
    case A20_SIOCGIFDSTADDR: case A20_SIOCSIFDSTADDR: case A20_SIOCGIFBRDADDR:
    case A20_SIOCSIFBRDADDR: case A20_SIOCGIFNETMASK: case A20_SIOCSIFNETMASK:
    case A20_SIOCGIFMTU: case A20_SIOCSIFMTU: case A20_SIOCGIFHWADDR:
    case A20_SIOCGIFINDEX:
        return net_vfile_ifreq_ioctl(arg, req);
    }
    return -ENOTTY;
}

static vfile_ops_t g_net_ops = {
    .read = net_vfile_read,
    .write = net_vfile_write,
    .lseek = net_vfile_lseek,
    .ioctl = net_vfile_ioctl,
    .poll = net_poll_file,
    .poll_sources = net_vfile_poll_sources,
    .close = net_socket_close_file,
};

int net_is_socket_vfile(struct vfile *vf)
{
    return vf && vf->ops == &g_net_ops && vf->priv;
}

net_socket_t *net_socket_from_vfile(vfile_t *vf) {
    return (vf && net_is_socket_vfile(vf) && vf->priv) ?
           (net_socket_t *)vf->priv : NULL;
}

net_socket_t *net_socket_from_file(int gfd) {
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return NULL;
    net_socket_t *s = (vf->ops == &g_net_ops && vf->priv) ?
                      (net_socket_t *)vf->priv : NULL;
    vfs_put_file_ref(gfd, vf);
    return s;
}

/* Resolve exactly the vfile observed under the current fd table lock, then
 * pin its socket before allowing the fd-table reference to be dropped. The
 * caller retains both references until the syscall is entirely finished, so
 * closing/reusing the numeric descriptor cannot run the socket close callback
 * while the syscall still uses the object. */


/*
 * Push an event to every event queue watching this socket's global fd
 * (Native ABI event source wiring, docs/native-abi/09-… §5).  Cheap when no
 * Native process watches the socket.  Called from the net core; safe with a
 * net lock held (a20_event_notify takes its own IRQ-safe locks).
 */
void net_event_notify(net_socket_t *s, uint32_t event, uint64_t data0,
                      uint64_t data1)
{
    if (!s || !s->vf)
        return;
    a20_event_notify(s->vf, A20_OBJ_SOCKET, event,
                     data0, data1);
}

/*
 * Rust lifecycle.
 *
 * What is left here is the primitive half: the view, the socket lock and the
 * address-ordered pair, the lwIP endpoint destroy, the channel release, the
 * wait-queue collect/wake/flush, the receive-queue detach, the registry
 * unregister and the socket free, the vfile allocator and the fd-table install.
 * Which of those to call, in what order, and which references a teardown owes
 * are the Rust transaction's decisions.
 */

void a20_net_close_view_init(void *sp, net_close_view_t *out)
{
    net_socket_t *s = (net_socket_t *)sp;
    memset(out, 0, sizeof(*out));
    if (!s)
        return;
    out->s = s;
    out->closed = &s->closed;
    out->peer_closed = &s->peer_closed;
    out->peer = (void **)&s->peer;
    out->sock_type = &s->type;
    out->accept_head = (void **)&s->accept_head;
    out->accept_tail = (void **)&s->accept_tail;
    out->accept_count = &s->accept_count;
    out->accept_next = (void **)&s->accept_next;
    out->refs = &s->refs;
    out->vf_priv = NULL; /* the caller that owns the vfile fills this in */
}

void a20_net_close_lock(const net_close_view_t *v, uint64_t *flags)
{
    *flags = net_sock_lock((net_socket_t *)v->s);
}

void a20_net_close_unlock(const net_close_view_t *v, uint64_t flags)
{
    net_sock_unlock((net_socket_t *)v->s, flags);
}

void a20_net_close_lock_pair(const net_close_view_t *a,
                             const net_close_view_t *b,
                             const net_close_view_t **lo,
                             const net_close_view_t **hi, uint64_t *flags)
{
    net_socket_t *sa = a ? (net_socket_t *)a->s : NULL;
    net_socket_t *sb = b ? (net_socket_t *)b->s : NULL;
    net_sock_pair_t p = net_sock_lock2(sa, sb);
    *lo = (a && p.lo == sa) ? a : b;
    *hi = (!p.hi) ? NULL : ((a && p.hi == sa) ? a : b);
    *flags = p.flags;
}

void a20_net_close_unlock_pair(const net_close_view_t *lo,
                               const net_close_view_t *hi, uint64_t flags)
{
    net_sock_pair_t p;
    p.lo = lo ? (net_socket_t *)lo->s : NULL;
    p.hi = hi ? (net_socket_t *)hi->s : NULL;
    p.flags = flags;
    net_sock_unlock2(p);
}

void a20_net_close_inet_destroy(const net_close_view_t *v)
{
    net_inet_socket_destroy((net_socket_t *)v->s);
}

void a20_net_close_channel_release(const net_close_view_t *v)
{
    net_socket_t *s = (net_socket_t *)v->s;
    if (s->ch_ep) {
        a20_channel_ep_release(s->ch_ep);
        s->ch_ep = NULL;
    }
}

void *a20_net_close_waitq(const net_close_view_t *v, int which)
{
    net_socket_t *s = (net_socket_t *)v->s;
    switch (which) {
    case 0: return &s->accept_waitq;
    case 1: return &s->read_waitq;
    default: return &s->write_waitq;
    }
}

int a20_net_close_collect(void *q, int reason, void *wake_q)
{
    return net_wait_queue_collect_all_locked(
               (wait_queue_t *)q, (proc_wake_reason_t)reason,
               (proc_wake_q_t *)wake_q) ? 1 : 0;
}

void a20_net_close_wake(void *q, int reason)
{
    (void)wait_queue_wake_all((wait_queue_t *)q, 0,
                              (proc_wake_reason_t)reason);
}

void a20_net_close_flush(void *wake_q)
{
    (void)proc_wake_q_flush((proc_wake_q_t *)wake_q);
}

void a20_net_close_wake_q_reinit(void *wake_q)
{
    proc_wake_q_init((proc_wake_q_t *)wake_q);
}

void *a20_net_close_rxq_detach(const net_close_view_t *v)
{
    return net_rxq_detach_locked((net_socket_t *)v->s);
}

void a20_net_close_msg_free_chain(void *m)
{
    net_msg_t *msg = (net_msg_t *)m;
    while (msg) {
        net_msg_t *next = msg->next;
        net_msg_free(msg);
        msg = next;
    }
}

int a20_net_close_unregister(const net_close_view_t *v)
{
    return net_socket_unregister((net_socket_t *)v->s) ? 1 : 0;
}

void a20_net_close_socket_free(const net_close_view_t *v)
{
    net_socket_free((net_socket_t *)v->s);
}

void *a20_net_install_prepare(void *sp, int flags)
{
    net_socket_t *s = (net_socket_t *)sp;
    vfile_t *vf = vfile_alloc();
    if (!vf)
        return NULL;
    vf->flags = flags;
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_net_ops;
    vf->priv = s;
    return vf;
}

int a20_net_install_fdtable(void *vf, int flags)
{
    return fdtable_install_current_vfile((vfile_t *)vf, flags);
}

void a20_net_install_set_owner(void *sp, int gfd, void *vf)
{
    net_socket_t *s = (net_socket_t *)sp;
    s->gfd = gfd;
    s->vf = (struct vfile *)vf;
}

void a20_net_install_free(void *vf)
{
    vfile_free((vfile_t *)vf);
}

void *a20_net_fdtable_current_file(int fd)
{
    return fdtable_get_current_file_ref(fd);
}

void *a20_net_vfile_socket(void *vf)
{
    return net_socket_from_vfile((vfile_t *)vf);
}

void a20_net_vfile_put(void *vf)
{
    vfs_put_file((vfile_t *)vf);
}

void a20_net_socket_ref(void *sp)
{
    (void)net_socket_ref((net_socket_t *)sp);
}

int net_socket_close_file(vfile_t *vf)
{
    proc_wake_q_t wake_q;
    net_close_view_t v;
    if (!vf)
        return 0;
    proc_wake_q_init(&wake_q);
    a20_net_close_view_init(vf->priv, &v);
    v.vf_priv = &vf->priv;
    return a20_net_socket_close(&v, &wake_q);
}

int net_socket_install_file(net_socket_t *s, int flags)
{
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    return a20_net_socket_install(s, flags, &wake_q);
}

int net_socket_pin_current_fd(int local_fd, vfile_t **out_vf,
                              net_socket_t **out_sock)
{
    return a20_net_socket_pin_current_fd(local_fd, (void **)out_vf,
                                         (void **)out_sock);
}

