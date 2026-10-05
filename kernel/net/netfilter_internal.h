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

/* Dotted-quad parse/format, defined in netfilter.c and shared so the NAT rule
 * parser does not carry a second copy that could disagree about what counts as
 * a valid address. */
int  netfilter_ipv4_from_str(const char *s, size_t len, uint32_t *out);
void netfilter_ipv4_to_str(uint32_t addr, char *buf, size_t bufsz);

#endif /* _NET_NETFILTER_INTERNAL_H */