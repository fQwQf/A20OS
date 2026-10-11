#ifndef _CLUSTER_UDP_H
#define _CLUSTER_UDP_H

#include "core/types.h"

#define A20_CLX_UDP_MTU 1472u
#define A20_CLX_UDP_PORT 44020u

/* Register the adapter without opening a socket. Routes start it lazily.
 * IPv4 next hops are six network-order bytes: address followed by port. */
void a20_clx_udp_init(void);
int a20_clx_udp_peer_add(const uint8_t node_id[16], const uint8_t *nh,
                         uint32_t nh_len);
void a20_clx_udp_peer_del(const uint8_t *nh, uint32_t nh_len);

#endif
