#include "core/progress.h"

#include "net/lwip_stack.h"
#include "net/net_config.h"
#include "net/socket_internal.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/net/virtio_net.h"
#include "lwip/timeouts.h"

/*
 * IO_PROGRESS_SERVICE (event-driven model):
 * - Block and network device completions are now driven by IRQ handlers.
 * - kernel_progress_timer_tick() runs from the periodic timer interrupt and
 *   advances lwIP timeouts only; it does not poll device rings.
 * - kernel_progress_run_bottom_halves() drains the socket deferred bottom-half
 *   ring; it is event-driven (atomic pending flags) and may be called from the
 *   scheduler / idle loop without touching devices.
 * - kernel_progress_poll() is retained as a compatibility hook for platforms
 *   where PCI virtio does not deliver IRQs (x86_64/loongarch64).  It polls
 *   block completions and network RX rings without sleeping.
 */
void kernel_progress_poll(kernel_progress_reason_t reason)
{
    (void)reason;
    virtio_blk_poll_all();
    virtio_net_poll_rx_all();
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
    kernel_progress_poll(KERNEL_PROGRESS_SCHED);
    net_inet_bottom_half_process_all();
}
