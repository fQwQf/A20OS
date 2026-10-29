/* Backend execution boundary. Socket and control adapters evolve separately;
 * no PCB, pbuf, mbuf, driver implementation or lane type crosses this API. */
#ifndef A20_NET_STACK_H
#define A20_NET_STACK_H

#include "core/types.h"
#include "net/net_config.h"
#include "net/transport.h"
#include "net/tun.h"

struct device;

typedef struct net_stack_ops {
    const net_transport_ops_t *transport;
    const char *name;
    void (*init)(void);
    void (*attach_interfaces)(void);
    void (*receive_progress)(unsigned budget);
    /* IRQ-safe borrowed device identity; backend chooses staging/locking.
     * Driver acknowledges IRQ and drops its queue lock before calling. */
    void (*device_interrupt)(const struct device *);
    void (*protocol_progress)(unsigned budget);
    void (*timer_tick)(void);
    int (*work_pending)(void);
    int (*tun_create)(net_tun_emit_t, void *);
    void (*tun_destroy)(unsigned);
    int (*tun_inject)(unsigned, const uint8_t *, size_t);
    /* Wire-order IPv4 address, public interface index, Ethernet address.
     * Dynamic entries use the backend's ordinary neighbor expiry policy. */
    int (*neighbor_update_v4)(uint32_t ifindex, uint32_t address,
                              const uint8_t mac[6]);
    int (*interface_snapshot)(void *out, int maximum);
    int (*status)(char *, size_t);
    int (*format_memory)(char *, size_t);
    int (*format_devices)(char *, size_t);
    int (*format_routes)(char *, size_t);
    void (*poll_waiter)(void);
    int (*if_up)(unsigned);
    int (*owns_ipv4)(uint32_t wire_address);
    int (*if_mtu)(unsigned);
    int (*if_default_index)(void);
    int (*if_hwaddr)(unsigned, uint8_t *);
    int (*packet_tx)(unsigned, const uint8_t *, size_t);
    void (*config_snapshot)(a20_net_config_t *out);
    int (*if_set_mtu)(unsigned, uint16_t);
    int (*if_set_flags)(unsigned, uint32_t, uint32_t);
    int (*if_get_addr)(unsigned, uint8_t *, uint8_t *, uint8_t *);
    int (*if_set_addr)(unsigned, const uint8_t *, const uint8_t *, const uint8_t *);
    int (*if_set_addr6)(unsigned, const uint8_t *, uint8_t);
    int (*if_del_addr6)(unsigned, const uint8_t *);
} net_stack_ops_t;

/* The selected backend is immutable for this kernel's initial network domain.
 * Switching a live connection or silently falling back is forbidden. These
 * calls operate on bounded batches, not once per packet. */
const char *net_stack_name(void);
const net_transport_ops_t *net_stack_transport(void);
void net_stack_init(void);
void net_stack_attach_interfaces(void);
void net_stack_receive_progress(unsigned budget);
void net_stack_device_interrupt(const struct device *);
void net_stack_protocol_progress(unsigned budget);
void net_stack_timer_tick(void);
int net_stack_work_pending(void);
int net_stack_tun_create(net_tun_emit_t, void *);
void net_stack_tun_destroy(unsigned);
int net_stack_tun_inject(unsigned, const uint8_t *, size_t);
int net_stack_status(char *, size_t);
int net_stack_format_memory(char *, size_t);
int net_stack_format_devices(char *, size_t);
int net_stack_format_routes(char *, size_t);
void net_stack_poll_waiter(void);
int net_stack_if_up(unsigned);
int net_stack_owns_ipv4(uint32_t wire_address);
int net_stack_if_mtu(unsigned);
int net_stack_if_default_index(void);
int net_stack_if_hwaddr(unsigned, uint8_t *);
int net_stack_packet_tx(unsigned, const uint8_t *, size_t);
int net_stack_interface_snapshot(void *, int);
void net_stack_config_snapshot(a20_net_config_t *);
int net_stack_if_set_mtu(unsigned, uint16_t);
int net_stack_if_set_flags(unsigned, uint32_t, uint32_t);
int net_stack_if_get_addr(unsigned, uint8_t *, uint8_t *, uint8_t *);
int net_stack_if_set_addr(unsigned, const uint8_t *, const uint8_t *, const uint8_t *);
int net_stack_if_set_addr6(unsigned, const uint8_t *, uint8_t);
int net_stack_if_del_addr6(unsigned, const uint8_t *);
int net_stack_neighbor_update_v4(uint32_t ifindex, uint32_t address,
                                  const uint8_t mac[6]);

#endif
