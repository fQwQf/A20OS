/*
 * Shared internals of the netfilter module (kernel/net/netfilter.c and
 * kernel/net/netfilter_nat.c).  Not a public header: nothing outside
 * kernel/net/ includes this.
 */
#ifndef _NET_NETFILTER_INTERNAL_H
#define _NET_NETFILTER_INTERNAL_H

#include "core/lock.h"
#include "core/types.h"

/*
 * Serialises writers of the two rule tables -- the filter table and the NAT
 * table.  Both are written only from procfs writes, and both are read from the
 * packet path, which runs under g_lwip_lock and must not take a second lock;
 * see the locking note at the top of kernel/include/net/netfilter.h for why
 * the conntrack table deliberately does *not* use this one.
 *
 * Defined in netfilter.c because that file owns the filter table's writer
 * discipline; the NAT table shares it so the two /proc surfaces cannot
 * interleave writes in a way that would make the ordering between them
 * ambiguous.
 */
extern spinlock_t g_netfilter_lock;

/* netfilter_ipv4_from_str / netfilter_ipv4_to_str are declared in
 * kernel/include/net/netfilter.h: procfs parses a dotted quad for "ctinject"
 * too, and duplicating the declaration here would only mean two places to
 * forget to update. */

#endif /* _NET_NETFILTER_INTERNAL_H */