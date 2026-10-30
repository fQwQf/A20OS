/* Public network-domain facade.
 *
 * The rest of the kernel calls these helpers; it never dereferences a
 * backend module or a protocol type.  The implementation lives in
 * kernel/net/core/stack.c and dispatches to the selected net_backend_t.
 *
 * See net/backend.h for the backend contract and docs/net/backend-interface.md
 * for the ownership rules.
 */
#ifndef A20_NET_STACK_H
#define A20_NET_STACK_H

#include "core/types.h"
#include "net/backend.h"

/* Selection and identity.  net_stack_init() selects the backend named by the
 * instance configuration and calls its cycle->init.  A missing backend is
 * reported loudly and leaves the domain unavailable; it never falls back. */
void net_stack_init(void);
void net_stack_attach_interfaces(void);
const net_backend_t *net_stack_backend(void);
int  net_stack_available(void);
const char *net_stack_name(void);
uint32_t net_stack_caps(void);
int  net_stack_has(uint32_t caps);
int  net_stack_format_caps(char *buf, size_t len);

/* Progress and device entry points.  All are bounded and safe to call when
 * the domain is unavailable (they become no-ops). */
void net_stack_receive_progress(unsigned budget);
void net_stack_device_interrupt(const struct device *);
void net_stack_protocol_progress(unsigned budget);
void net_stack_timer_tick(void);
int  net_stack_work_pending(void);
void net_stack_poll_waiter(void);

/* TUN and L2 bypass. */
int  net_stack_tun_create(net_tun_emit_t emit, void *ctx);
void net_stack_tun_destroy(unsigned index);
int  net_stack_tun_inject(unsigned index, const uint8_t *packet, size_t len);
int  net_stack_packet_tx(unsigned index, const uint8_t *frame, size_t len);

/* Interface and address transactions. */
int  net_stack_interface_snapshot(void *out, int maximum);
int  net_stack_if_up(unsigned index);
int  net_stack_if_default_index(void);
int  net_stack_owns_ipv4(uint32_t wire_address);
int  net_stack_if_mtu(unsigned index);
int  net_stack_if_hwaddr(unsigned index, uint8_t mac[6]);
int  net_stack_if_set_mtu(unsigned index, uint16_t mtu);
int  net_stack_if_set_flags(unsigned index, uint32_t value, uint32_t mask);
int  net_stack_if_get_addr(unsigned index, uint8_t *addr, uint8_t *mask,
                           uint8_t *gateway);
int  net_stack_if_set_addr(unsigned index, const uint8_t *addr,
                           const uint8_t *mask, const uint8_t *gateway);
int  net_stack_if_set_addr6(unsigned index, const uint8_t *addr,
                            uint8_t prefix);
int  net_stack_if_del_addr6(unsigned index, const uint8_t *addr);
int  net_stack_route_update_v4(const uint8_t *dst, uint8_t prefix,
                               const uint8_t *gateway, uint16_t index,
                               int remove);
int  net_stack_neighbor_update_v4(uint32_t index, uint32_t address,
                                  const uint8_t mac[6]);
uint32_t net_backend_interface_ipv4(int index);

/* Configuration and observability. */
void net_stack_config_snapshot(a20_net_config_t *out);
int  net_stack_status(char *buf, size_t len);
int  net_stack_format_memory(char *buf, size_t len);
int  net_stack_format_devices(char *buf, size_t len);
int  net_stack_format_routes(char *buf, size_t len);

#endif /* A20_NET_STACK_H */
