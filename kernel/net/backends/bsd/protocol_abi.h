/* Fixed-width values shared by the A20-side adapter and the BSD-namespace
 * port.  It deliberately includes nothing: the A20 side already has
 * core/types.h and the BSD side already has sys/types.h, and pulling either
 * into the other's namespace is exactly what this boundary exists to avoid. */
#ifndef A20_NET_BSD_PROTOCOL_ABI_H
#define A20_NET_BSD_PROTOCOL_ABI_H

typedef struct a20_bsd_pktinfo {
    uint8_t  valid;       /* 1 when at least one cmsg supplied metadata */
    uint8_t  family;      /* AF_INET / AF_INET6 */
    uint8_t  hoplimit;
    uint8_t  tclass;
    uint32_t ifindex;
    uint8_t  address[16]; /* IPv4 dst / IPv6 pktinfo address */
} a20_bsd_pktinfo_t;

/* Private to the BSD port. Both Ethernet and TUN own a FreeBSD ifnet. */
int port_linux_errno(int error);
struct ifnet;
int port_if_set_ipv6(struct ifnet *ifp, const uint8_t address[16],
                     uint8_t prefix, int add, uint32_t valid,
                     uint32_t preferred, int exclusive);

#endif

/* TUN callback shapes.  They mirror net/tun.h without including A20 headers
 * in the BSD namespace. */
typedef size_t (*a20_bsd_tun_reader_t)(void *source, void *destination,
                                       size_t length);
typedef int (*a20_bsd_tun_emit_t)(void *context, size_t length,
                                  a20_bsd_tun_reader_t reader, void *source);

typedef struct a20_bsd_tun_info {
    char     name[16];
    uint32_t mtu;
    uint32_t flags;
    uint8_t  ip[4], mask[4], gw[4];
    uint8_t  has_ip;
} a20_bsd_tun_info_t;

typedef struct a20_bsd_ready {
    int      fd;
    uint32_t flags;
    void    *tag;
} a20_bsd_ready_t;
#define A20_BSD_READY_EOF 1u
