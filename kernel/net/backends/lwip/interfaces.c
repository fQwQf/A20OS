#include "tun_private.h"
#include "native.h"
#include "net/socket/socket_internal.h"
#include "net/tun.h"
#include "net/lwip_stack.h"
#include "net/lwip_stack_state.h"
#include "net/stack.h"
#include "proc/userns.h"
#include "core/string.h"
#include "core/stdio.h"
#include "sys/usercopy.h"

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
    if (!net_tun_interface_name(netif_get_index(nif), ifr->ifr_name,
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
    n = netif_get_by_index((u8_t)net_tun_interface_by_name(name));
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
    size_t offset = family == A20_AF_INET ? 4 : 2;
    if (n > sizeof(out) - offset) n = sizeof(out) - offset;
    memcpy(out + offset, bytes, n);
    return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, out, sizeof(out)) < 0
           ? -EFAULT : 0;
}

static int net_vfile_ifreq_ioctl(void *uarg, unsigned long req)
{
    struct a20_ifreq ifr;
    if (copy_from_user(&ifr, uarg, sizeof(ifr)) < 0) return -EFAULT;
    ifr.ifr_name[A20_IFNAMSIZ - 1] = 0;
    uint64_t lf = a20_lwip_lock();
    struct netif *n = req == A20_SIOCGIFNAME
        ? netif_get_by_index((u8_t)ifr.ifr_ifru.ivalue) : net_ifreq_lookup(ifr.ifr_name);
    if (!n || (req == A20_SIOCGIFNAME &&
               (int)netif_get_index(n) != ifr.ifr_ifru.ivalue)) {
        a20_lwip_unlock(lf); return -ENODEV;
    }
    unsigned index = netif_get_index(n);
    if (req == A20_SIOCSIFADDR || req == A20_SIOCSIFNETMASK) {
        if (ifr.ifr_ifru.addr.sa_family != A20_AF_INET) {
            a20_lwip_unlock(lf); return -EINVAL;
        }
        ip4_addr_t value = { .addr = ifr.ifr_ifru.addr.sa_addr };
        if (req == A20_SIOCSIFADDR) netif_set_ipaddr(n, &value);
        else netif_set_netmask(n, &value);
        uint8_t addr[4], mask[4];
        memcpy(addr, netif_ip4_addr(n), 4); memcpy(mask, netif_ip4_netmask(n), 4);
        a20_lwip_unlock(lf);
        a20_lwip_stack_notify_addr(index, addr, mask);
        return 0;
    }
    /* All output is copied by value while serialized with interface removal.
     * No backend pointer or execution lock crosses a potentially faulting copy. */
    struct netif snapshot = *n;
    if (req == A20_SIOCGIFNAME) {
        if (!net_tun_interface_name(index, ifr.ifr_name, sizeof(ifr.ifr_name)))
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%c%c%u", n->name[0], n->name[1], n->num);
    }
    a20_lwip_unlock(lf);
    n = &snapshot;
    switch (req) {
    case A20_SIOCGIFNAME:
        return copy_to_user(uarg, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    case A20_SIOCGIFHWADDR:
        return net_ifreq_put_addr(uarg, n->flags & NETIF_FLAG_ETHERNET ?
                                  A20_ARPHRD_ETHER : A20_ARPHRD_LOOPBACK,
                                  n->hwaddr, n->hwaddr_len);
    case A20_SIOCGIFADDR:
        return net_ifreq_put_addr(uarg, A20_AF_INET, netif_ip4_addr(n), 4);
    case A20_SIOCGIFNETMASK:
        return net_ifreq_put_addr(uarg, A20_AF_INET, netif_ip4_netmask(n), 4);
    case A20_SIOCGIFDSTADDR: case A20_SIOCGIFBRDADDR: {
        uint32_t broadcast = netif_ip4_addr(n)->addr | ~netif_ip4_netmask(n)->addr;
        return net_ifreq_put_addr(uarg, A20_AF_INET, &broadcast, 4);
    }
    case A20_SIOCGIFMTU: case A20_SIOCGIFINDEX: {
        int value = req == A20_SIOCGIFMTU ? n->mtu : (int)index;
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &value, sizeof(value)) < 0 ? -EFAULT : 0;
    }
    case A20_SIOCSIFMTU:
        if (ifr.ifr_ifru.ivalue < 0 || ifr.ifr_ifru.ivalue > 65535) return -EINVAL;
        return a20_lwip_if_set_mtu(index, (uint16_t)ifr.ifr_ifru.ivalue);
    case A20_SIOCGIFFLAGS: {
        short value = 0;
        if (n->flags & NETIF_FLAG_BROADCAST) value |= A20_IFF_BROADCAST;
        if (netif_ip4_addr(n)->addr == 0x0100007fU) value |= A20_IFF_LOOPBACK;
        if (n->flags & NETIF_FLAG_UP) value |= A20_IFF_UP;
        if (n->flags & NETIF_FLAG_LINK_UP) value |= A20_IFF_RUNNING;
        return copy_to_user((uint8_t *)uarg + A20_IFNAMSIZ, &value, sizeof(value)) < 0 ? -EFAULT : 0;
    }
    case A20_SIOCSIFFLAGS:
        return a20_lwip_if_set_flags(index, (uint16_t)ifr.ifr_ifru.flags, A20_IFF_UP);
    default: return -ENOTTY;
    }
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

int a20_lwip_transport_interface_ioctl(net_socket_t *s, unsigned long req, void *arg)
{
    if (req == 0x8955 /* SIOCSARP */) {
        /* Linux arpreq: three sockaddr values, flags and IFNAMSIZ name.
         * Support complete dynamic Ethernet neighbors, not permanent/proxy
         * entries. Names are resolved to values before calling the backend. */
        struct {
            uint8_t protocol[16], hardware[16];
            int flags;
            uint8_t netmask[16];
            char device[A20_IFNAMSIZ];
        } request;
        if (!s) return -ENOTSOCK;
        if (!userns_capable(proc_current(), s->net_ns->owner, CAP_NET_ADMIN))
            return -EPERM;
        if (!resource_ns_is_initial(s->net_ns)) return -EOPNOTSUPP;
        if (copy_from_user(&request, arg, sizeof(request)) < 0) return -EFAULT;
        uint16_t family, hardware;
        uint32_t address;
        memcpy(&family, request.protocol, sizeof(family));
        memcpy(&hardware, request.hardware, sizeof(hardware));
        memcpy(&address, request.protocol + 4, sizeof(address));
        if (family != AF_INET || hardware != A20_ARPHRD_ETHER) return -EINVAL;
        if (request.flags != 2 /* ATF_COM */) return -EOPNOTSUPP;
        request.device[A20_IFNAMSIZ - 1] = 0;
        if (!request.device[0]) return -ENODEV;
        uint64_t lf = a20_lwip_lock();
        struct netif *n = net_ifreq_lookup(request.device);
        uint32_t index = n ? netif_get_index(n) : 0;
        a20_lwip_unlock(lf);
        if (!index) return -ENODEV;
        return net_stack_neighbor_update_v4(index, address, request.hardware + 2);
    }
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

#include "net/interface.h"
int a20_lwip_interface_snapshot(void *outp, int max) {
    nl_link_out_t *out = outp;
    uint64_t lf = a20_lwip_lock();
    int n = 0;
    for (struct netif *ni = netif_list; ni && n < max; ni = ni->next) {
        nl_link_out_t *e = &out[n++];
        memset(e, 0, sizeof(*e));
        /* A TUN netif carries a user-chosen name ("tailscale0"), which does
         * not fit lwIP's 2-char + num scheme; ask the service for it. */
        char tun_name[16];
        int is_tun = net_tun_interface_name(netif_get_index(ni), tun_name, sizeof(tun_name));
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

int a20_lwip_interface_mtu(unsigned index) {
    uint64_t flags = a20_lwip_lock();
    struct netif *n = index && index <= 255 ? netif_get_by_index((u8_t)index) : NULL;
    int result = n ? n->mtu : -ENODEV;
    a20_lwip_unlock(flags);
    return result;
}
