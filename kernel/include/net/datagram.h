/* Per-datagram values, borrowed only for the send call. Addresses retain their
 * wire bytes; ifindex is the public interface identity. No PCB state is saved,
 * temporarily overwritten, or shared between concurrent senders. */
#ifndef A20_NET_DATAGRAM_H
#define A20_NET_DATAGRAM_H
#include "core/types.h"
typedef struct net_datagram_control {
    uint32_t ifindex;   /* Zero selects routing/bound-interface policy. */
    uint32_t source_v4; /* Zero selects the interface's local address. */
} net_datagram_control_t;
#endif
