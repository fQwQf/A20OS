/* Link-time selection keeps kernel progress independent of protocol and NIC
 * implementation. Unavailable backends are rejected by the build manifest. */
#include "net/stack.h"
#include "core/errno.h"
#include "core/stdio.h"

extern const net_stack_ops_t a20_network_stack_ops;
static const net_stack_ops_t *const stack = &a20_network_stack_ops;

const char *net_stack_name(void) { return stack->name; }
const net_transport_ops_t *net_stack_transport(void) { return stack->transport; }
void net_stack_init(void) { stack->init(); }
void net_stack_attach_interfaces(void) { stack->attach_interfaces(); }
void net_stack_receive_progress(unsigned budget) {
    stack->receive_progress(budget);
}
void net_stack_protocol_progress(unsigned budget) {
    stack->protocol_progress(budget);
}
void net_stack_timer_tick(void) { stack->timer_tick(); }
int net_stack_work_pending(void) { return stack->work_pending(); }
int net_stack_interface_snapshot(void *out, int max) { return stack->interface_snapshot(out, max); }
void net_stack_config_snapshot(a20_net_config_t *out) { stack->config_snapshot(out); }
int net_stack_if_set_mtu(unsigned i, uint16_t m) { return stack->if_set_mtu(i, m); }
int net_stack_if_set_flags(unsigned i, uint32_t v, uint32_t m) { return stack->if_set_flags(i, v, m); }
int net_stack_if_get_addr(unsigned i, uint8_t *a, uint8_t *m, uint8_t *g) { return stack->if_get_addr(i, a, m, g); }
int net_stack_if_set_addr(unsigned i, const uint8_t *a, const uint8_t *m, const uint8_t *g) { return stack->if_set_addr(i, a, m, g); }
int net_stack_if_set_addr6(unsigned i, const uint8_t *a, uint8_t p) { return stack->if_set_addr6(i, a, p); }
int net_stack_if_del_addr6(unsigned i, const uint8_t *a) { return stack->if_del_addr6(i, a); }
int net_stack_neighbor_update_v4(uint32_t ifindex, uint32_t address,
                                  const uint8_t mac[6]) {
    return stack->neighbor_update_v4
        ? stack->neighbor_update_v4(ifindex, address, mac) : -EOPNOTSUPP;
}

int net_stack_status(char *b, size_t n) {
    if (!n) return 0;
    int used = snprintf(b, n, "backend: %s\n", stack->name);
    if (used < 0) return 0;
    if ((size_t)used >= n) return (int)n - 1;
    return used + stack->status(b + used, n - (size_t)used);
}
void net_stack_poll_waiter(void) { stack->poll_waiter(); }
int net_stack_if_up(unsigned i) { return stack->if_up(i); }
int net_stack_if_default_index(void) { return stack->if_default_index(); }
int net_stack_if_hwaddr(unsigned i, uint8_t *a) { return stack->if_hwaddr(i, a); }
int net_stack_packet_tx(unsigned i, const uint8_t *p, size_t n) { return stack->packet_tx(i, p, n); }
