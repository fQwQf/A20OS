/* Backend execution boundary. Socket and control adapters evolve separately;
 * no PCB, pbuf, mbuf, driver implementation or lane type crosses this API. */
#ifndef A20_NET_STACK_H
#define A20_NET_STACK_H

#include "core/types.h"

typedef struct net_stack_ops {
    const char *name;
    void (*init)(void);
    void (*attach_interfaces)(void);
    void (*receive_progress)(unsigned budget);
    void (*protocol_progress)(unsigned budget);
    void (*timer_tick)(void);
    int (*work_pending)(void);
    /* Wire-order IPv4 address, public interface index, Ethernet address.
     * Dynamic entries use the backend's ordinary neighbor expiry policy. */
    int (*neighbor_update_v4)(uint32_t ifindex, uint32_t address,
                              const uint8_t mac[6]);
} net_stack_ops_t;

/* The selected backend is immutable for this kernel's initial network domain.
 * Switching a live connection or silently falling back is forbidden. These
 * calls operate on bounded batches, not once per packet. */
const char *net_stack_name(void);
void net_stack_init(void);
void net_stack_attach_interfaces(void);
void net_stack_receive_progress(unsigned budget);
void net_stack_protocol_progress(unsigned budget);
void net_stack_timer_tick(void);
int net_stack_work_pending(void);
int net_stack_neighbor_update_v4(uint32_t ifindex, uint32_t address,
                                  const uint8_t mac[6]);

#endif
