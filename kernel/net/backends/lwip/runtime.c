/* lwIP owns its execution model. A20OS scheduling only sees bounded protocol
 * progress and pending work, including socket events published by callbacks. */
#include "net/stack.h"
#include "net/lwip_stack.h"
#include "net/socket/socket_internal.h"

static void protocol_progress(unsigned budget) {
    (void)budget;
    A20_LWIP_LANE_RX_POLL(budget);
    net_inet_bottom_half_process_all();
}

static void timer_tick(void) {
#if CONFIG_NET_LANES > 1
    a20_lwip_signal_timer_pending();
#else
    uint64_t flags = a20_lwip_lock();
    a20_lwip_poll_timers_locked();
    a20_lwip_unlock(flags);
#endif
}

static int work_pending(void) {
    return a20_lwip_rx_pending_any() || net_bh_any_pending();
}

const net_stack_ops_t a20_lwip_stack_ops = {
    .name = "lwip",
    .init = a20_lwip_init,
    .attach_interfaces = a20_lwip_attach_netifs,
    .receive_progress = a20_lwip_rx_progress,
    .protocol_progress = protocol_progress,
    .timer_tick = timer_tick,
    .work_pending = work_pending,
};
