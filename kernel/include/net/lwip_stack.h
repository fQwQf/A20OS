#ifndef _NET_LWIP_STACK_H
#define _NET_LWIP_STACK_H

#include "core/types.h"

/* LWIP_PROGRESS_API: use a20_lwip_poll() only through core/progress from
 * scheduler/idle code; socket paths may poll explicitly while waiting on network
 * state. */

void a20_lwip_init(void);
void a20_lwip_poll(void);
void a20_lwip_poll_locked(void);
void a20_lwip_process_netif_irq_locked(int net_idx);
uint64_t a20_lwip_lock(void);
void a20_lwip_unlock(uint64_t flags);
int  a20_lwip_format_status(char *buf, size_t bufsz);
int  a20_lwip_rx_pending_any(void);
void a20_lwip_signal_rx_pending(void);

#endif /* _NET_LWIP_STACK_H */
