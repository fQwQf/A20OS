/* Host side of the BSD packet policy boundary. Capture observes the original
 * Ethernet frame, including fragments. Policy sees the complete IPv4 packet. */
#include "net/netfilter.h"
#include "nic.h"

/* Keep the cross-namespace bridge independent of the socket frontend. */
int net_packet_bound_count(void);
void net_packet_rx_defer(unsigned ifindex, const uint8_t *frame, size_t len);

void a20_bsd_capture_input(const void *frame, size_t len, int slot)
{
    if (net_packet_bound_count())
        net_packet_rx_defer(nic_index_for_slot(slot), frame, len);
}

int a20_bsd_netfilter_busy(void)
{
    return netfilter_busy();
}

int a20_bsd_netfilter_input(void *frame, size_t len, int ifindex)
{
    return netfilter_input(frame, len, ifindex);
}

int a20_bsd_netfilter_output(void *frame, size_t len, int ifindex)
{
    return netfilter_output(frame, len, ifindex);
}
