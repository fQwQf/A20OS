#ifndef _CLUSTER_UDP_H
#define _CLUSTER_UDP_H

#include "core/types.h"

#define A20_CLX_UDP_MTU 1472u
#define A20_CLX_UDP_PORT 44020u

/* Register the adapter without opening a socket. Routes start it lazily.
 * IPv4 next hops are six network-order bytes: address followed by port. */
void a20_clx_udp_init(void);
/* UDP readiness wakes the poll owner early; deadline still bounds UART and
 * link-control progress. No scheduler calls under transport/socket locks. */
void a20_clx_udp_wait(uint64_t deadline);
uint32_t a20_clx_udp_logical_mtu(const uint8_t node_id[16]);
int a20_clx_udp_peer_add(const uint8_t node_id[16], const uint8_t *nh,
                         uint32_t nh_len);
void a20_clx_udp_peer_del(const uint8_t *nh, uint32_t nh_len);

#endif
