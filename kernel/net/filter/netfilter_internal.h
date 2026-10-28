/*
 * Shared internals of the netfilter module (kernel/net/filter/netfilter.c and
 * kernel/net/filter/netfilter_nat.c).  Not a public header: nothing outside
 * kernel/net/ includes this.
 */
#ifndef _NET_NETFILTER_INTERNAL_H
#define _NET_NETFILTER_INTERNAL_H

#include "core/lock.h"
#include "core/types.h"

/* Serialize configuration writers for both rule tables. Conntrack/NAT
 * separately own an IRQ-safe CT lock; the only order is writer -> CT. Packet
 * hooks do not acquire this configuration writer lock. */
extern spinlock_t g_netfilter_lock;

/* netfilter_ipv4_from_str / netfilter_ipv4_to_str are declared in
 * kernel/include/net/netfilter.h: procfs parses a dotted quad for "ctinject"
 * too, and duplicating the declaration here would only mean two places to
 * forget to update. */

#endif /* _NET_NETFILTER_INTERNAL_H */