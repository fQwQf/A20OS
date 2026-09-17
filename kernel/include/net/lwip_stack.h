#ifndef _NET_LWIP_STACK_H
#define _NET_LWIP_STACK_H

#include "core/types.h"

/* LWIP_PROGRESS_API: use a20_lwip_poll() only through core/progress from
 * scheduler/idle code; socket paths may poll explicitly while waiting on network
 * state. */

void a20_lwip_init(void);
void a20_lwip_attach_netifs(void);
void a20_lwip_poll(void);
void a20_lwip_poll_locked(void);
void a20_lwip_process_netif_irq_locked(int net_idx);
uint64_t a20_lwip_lock(void);
void a20_lwip_unlock(uint64_t flags);
int  a20_lwip_format_status(char *buf, size_t bufsz);
int  a20_lwip_rx_pending_any(void);
void a20_lwip_signal_rx_pending(void);

/* AF_PACKET L2 access.  ifindex is the netif index (see
 * net_packet_ifindex_by_name in socket_internal.h); frame is a full
 * Ethernet frame including the 14-byte header. */
int  a20_lwip_packet_tx(unsigned ifindex, const uint8_t *frame, size_t len);
int  a20_lwip_if_hwaddr(unsigned ifindex, uint8_t out[8]);
int  a20_lwip_if_up(unsigned ifindex);
int  a20_lwip_if_default_index(void);

#endif /* _NET_LWIP_STACK_H */
