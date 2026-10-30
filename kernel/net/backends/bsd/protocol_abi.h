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

#endif
