/* A20OS policy bridge for the BSD data path.
 *
 * ff_veth owns the mbuf/token queue, but filtering, conntrack and NAT policy
 * belong to the common A20OS layer.  These two entry points are called from the
 * BSD-namespace port with borrowed, writable frame bytes; they return
 * NETFILTER_ACCEPT/DROP so the port can free its token/mbuf on the drop path.
 */
#include "net/netfilter.h"

int a20_bsd_netfilter_input(void *frame, size_t len, int ifindex)
{
    if (!frame || !len || !netfilter_busy())
        return NETFILTER_ACCEPT;
    return netfilter_input((uint8_t *)frame, len, ifindex);
}

int a20_bsd_netfilter_output(void *frame, size_t len, int ifindex)
{
    if (!frame || !len || !netfilter_busy())
        return NETFILTER_ACCEPT;
    return netfilter_output((uint8_t *)frame, len, ifindex);
}
