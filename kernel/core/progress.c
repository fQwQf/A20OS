#include "core/progress.h"

#include "net/lwip_stack.h"
#include "net/net_config.h"
#include "net/socket_internal.h"
#include "lwip/timeouts.h"

/*
 * IO_PROGRESS_SERVICE (event-driven model):
 * - Block and network device completions are now driven by IRQ handlers.
 * - kernel_progress_timer_tick() runs from the periodic timer interrupt and
 *   advances lwIP timeouts only; it does not poll block or network rings.
 * - kernel_progress_run_bottom_halves() drains the socket deferred bottom-half
 *   ring; it is event-driven (atomic pending flags) and may be called from the
 *   scheduler / idle loop without touching devices.
 * - kernel_progress_poll() is retained as a compatibility hook but performs
 *   no hot-path device polling.
 */
void kernel_progress_poll(kernel_progress_reason_t reason)
{
    (void)reason;
}

void kernel_progress_timer_tick(void)
{
    uint64_t flags = a20_lwip_lock();
    sys_check_timeouts();
    a20_net_config_sync_from_lwip();
    a20_lwip_unlock(flags);
}

void kernel_progress_run_bottom_halves(void)
{
    net_inet_bottom_half_process_all();
}
