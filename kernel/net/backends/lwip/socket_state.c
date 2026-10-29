#include "native.h"
#include "mm/slab.h"
#include "net/lwip_stack.h"
#include "core/string.h"

/* Attach only before publication; accepted children get their own state before
 * adopting a PCB. The attachment survives close until the last socket ref. */
int net_inet_state_attach(net_socket_t *s) {
    if (s->transport_private) return 1;
    lwip_socket_t *state = kmalloc(sizeof(*state));
    if (!state) return 0;
    memset(state, 0, sizeof(*state));
    state->lane = net_lane_of_cpu(cpu_current_id());
    s->transport_private = state;
    return 1;
}
void a20_lwip_transport_state_release(net_socket_t *s) {
    kfree(s->transport_private);
    s->transport_private = NULL;
}
unsigned a20_lwip_transport_flags(const net_socket_t *s) {
    if (!lw(s)) return 0;
    uint64_t flags = a20_lwip_lock();
    unsigned kinds = (lw(s)->tcp ? 1u : 0u) | (lw(s)->udp ? 2u : 0u) |
                     (lw(s)->raw ? 4u : 0u);
    a20_lwip_unlock(flags);
    return kinds;
}
int a20_lwip_transport_has_transport(const net_socket_t *s) {
    return a20_lwip_transport_flags(s) != 0;
}
int a20_lwip_transport_has_tcp(const net_socket_t *s) {
    return !!(a20_lwip_transport_flags(s) & 1);
}
