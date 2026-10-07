/*
 * loopback transport (docs/cluster/04-transports.md §2) -- WA-owned, all
 * tiers.  4-byte virtual-node-number link addresses; send copies the frame
 * into this kernel's RX upcall (zero loss, ordered, no shared buffer).
 */
#ifndef _CLUSTER_LOOPBACK_H
#define _CLUSTER_LOOPBACK_H

#include "core/types.h"
#include "cluster/transport.h"

void a20_clx_loopback_init(void);
/* a20_clx_transport_t.send shape.  next_hop = 4B virtual node number. */
int  a20_clx_loopback_send(void *ctx, const uint8_t *next_hop,
                           uint32_t nh_len, const void *frame, uint32_t len);
/* Map a received link address back to the hosted identity behind it
 * (self for A20_CLX_LOOPBACK_SELF_ADDR, else the virtual node registry).
 * Returns 0 or -1 for an unknown address. */
int  a20_clx_loopback_addr_node(uint32_t transport_id, const uint8_t *nh,
                                uint32_t nh_len, a20_node_id_t *out);

#endif /* _CLUSTER_LOOPBACK_H */
