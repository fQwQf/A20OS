#ifndef A20_NET_BSD_NIC_H
#define A20_NET_BSD_NIC_H
#include "core/types.h"

/* Slot zero is en2. Loopback owns public index 2, between the NIC slots. */
static inline unsigned nic_index_for_slot(int slot)
{
    return slot == 0 ? 1u : (unsigned)(slot + 2);
}

/* Translate a FreeBSD if_index carried by IP_RECVIF/IPV6_PKTINFO into the
 * stable A20 public index, and return the interface's configured IPv4 address.
 * Returns -ENODEV when the index is not one of the attached NICs. */
int a20_bsd_nic_public_ifindex(unsigned bsd_ifindex, uint8_t local[4],
                               uint32_t *public_index);
/* Interface zone translations run on the BSD executor. */
int a20_bsd_ifindex_to_bsd(unsigned index);
int a20_bsd_ifindex_to_public(unsigned index);
#endif
