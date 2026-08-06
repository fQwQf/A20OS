#include "core/progress.h"

#include "net/lwip_stack.h"
#include "net/net_config.h"
#include "net/socket_internal.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/net/virtio_net.h"
#include "lwip/timeouts.h"

/*
 * IO_PROGRESS_SERVICE (event-driven model):
 * - Block and network device completions are driven by IRQ handlers.  The
 *   block driver polls the used ring for a short hybrid window then parks on
 *   the completion IRQ; the network IRQ top-half raises an RX pending flag.
 * - kernel_progress_timer_tick() runs from the periodic timer interrupt and
 *   advances lwIP timeouts; it additionally runs the gated network RX drain
 *   as a periodic safety net so RX cannot stall if a device IRQ is ever lost.
 * - kernel_progress_run_bottom_halves() drains the socket deferred bottom-half
 *   ring (event-driven atomic pending flags) and the gated device progress.
 * - kernel_progress_poll() is the gated scheduler/idle bridge: it drains the
 *   block used rings cheaply and the network RX ring only when a device has
 *   signalled work (or a transport without an IRQ line still needs polling).
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
    /*
     * Periodic event-driven safety net: on IRQ-capable platforms the virtio
     * IRQ top-half raises an RX pending flag, so this gated drain only
     * acquires g_lwip_lock again when a device actually signalled work; a
     * poll-only transport keeps draining unconditionally.  This guarantees RX
     * cannot stall even if a device IRQ is ever lost, while keeping the
     * per-context-switch scheduler hot path free of the lock.
     */
    virtio_net_poll_rx_all();
}

void kernel_progress_run_bottom_halves(void)
{
    kernel_progress_poll(KERNEL_PROGRESS_SCHED);
    net_inet_bottom_half_process_all();
}
