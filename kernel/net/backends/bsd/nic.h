#ifndef A20_NET_BSD_NIC_H
#define A20_NET_BSD_NIC_H
#include "core/types.h"

/* Translate a FreeBSD if_index carried by IP_RECVIF/IPV6_PKTINFO into the
 * stable A20 public index, and return the interface's configured IPv4 address.
 * Returns -ENODEV when the index is not one of the attached NICs. */
int a20_bsd_nic_public_ifindex(unsigned bsd_ifindex, uint8_t local[4],
                               uint32_t *public_index);
#endif
