/* Link-time selection keeps kernel progress independent of protocol and NIC
 * implementation. Unavailable backends are rejected by the build manifest. */
#include "net/stack.h"

extern const net_stack_ops_t a20_lwip_stack_ops;
static const net_stack_ops_t *const stack = &a20_lwip_stack_ops;

const char *net_stack_name(void) { return stack->name; }
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
