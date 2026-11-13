#include "core/progress.h"
#include "core/smp_test.h"

#include "core/cpu.h"
#include "net/stack.h"
#include "net/profile.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/usb/usb.h"

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
 *
 * KERNEL_PROGRESS_PENDING_CONTRACT:
 * - kernel_progress_run_bottom_halves() runs on every scheduler and idle pass,
 *   so it must not speculate on work that has a cost to reject it.
 * - The device half of the bridge takes a global mutex and walks every
 *   registered device, and no device publishes an "I am idle" flag, so the
 *   bridge cannot ask the driver itself whether it is worth entering.
 * - A producer that creates work for this bridge therefore ORs
 *   KERNEL_PROGRESS_PENDING_DEVICE from that context (completion IRQ,
 *   submission, deferred wake).  OR-ing is idempotent, so producers racing
 *   each other and racing the consumer all still make progress.
 * - The gate is an optimization, never the only liveness path: the timer
 *   re-arms the bit on a bounded cadence, so a producer that misses its
 *   notification delays completion by at most one fallback interval instead of
 *   losing it.  Narrow the fallback once every producer is wired.
 * - The network RX half is deliberately NOT gated.  the netif poll-only
 *   transport contract requires kernel_progress_poll() to keep draining on
 *   every pass, and the drain already returns without taking g_lwip_lock
 *   unless a device signalled work.
 */

/* Fallback re-arm interval for the device bit, counted in timer IRQs.  It
 * bounds how long a completion can be deferred when a producer missed its
 * notification.  Scaling this by clock_ticks_per_sec() is wrong: that reports
 * the timer's *counter* frequency (1 GHz on qemu-virt), not the IRQ rate, so
 * the interval became ~10^8 IRQs and the bit was never re-armed in practice.
 * A fixed small count of IRQs is the honest unit. */
#define KERNEL_PROGRESS_FALLBACK_TICKS 8u

static uint32_t g_progress_pending;
static uint64_t g_progress_fallback_ticks;

void kernel_progress_note_pending(uint32_t bits) {
    __atomic_fetch_or(&g_progress_pending, bits, __ATOMIC_RELAXED);
}

static void kernel_progress_devices(void) {
    __atomic_fetch_and(&g_progress_pending, ~KERNEL_PROGRESS_PENDING_DEVICE,
                       __ATOMIC_RELEASE);
    driver_progress_class(DEV_CLASS_BLOCK);
    /* Registration and removal may call driver callbacks, so hotplug polling
     * runs only from this scheduler/idle process-context bridge. */
    usb_core_poll();
}

/* One CPU stages the single device RX ring. Protocol consumers run on every
 * CPU under independent owner lane locks; the ingress lock is not a core lock
 * in multi-lane builds. */
static void kernel_progress_net_rx(void) {
    if (cpu_current_id() == 0)
        net_stack_receive_progress(CONFIG_NET_RX_IRQ_BUDGET);
}

void kernel_progress_poll(kernel_progress_reason_t reason) {
    (void)reason;
    kernel_progress_devices();
    kernel_progress_net_rx();
}

void kernel_progress_timer_tick(void) {
#if defined(BRINGUP) && !defined(CONFIG_MCU)
    smp_selftest_note_timer_irq();
#endif
    /* CPU 0 publishes the timer hint; multi-lane work runs in process context. */
    if (cpu_current_id() != 0)
        return;
    net_stack_timer_tick();
    /* The device bit is the liveness floor for completion-polled drivers:
     * re-arm it here so the scheduler hot path can skip the device walk. */
    if (++g_progress_fallback_ticks >= KERNEL_PROGRESS_FALLBACK_TICKS) {
        g_progress_fallback_ticks = 0;
        kernel_progress_note_pending(KERNEL_PROGRESS_PENDING_DEVICE);
    }
    /*
     * Periodic event-driven safety net: on IRQ-capable platforms the virtio
     * IRQ top-half raises an RX pending flag, so this gated drain only
     * acquires g_lwip_lock again when a device actually signalled work; a
     * poll-only transport keeps draining unconditionally.  This guarantees RX
     * cannot stall even if a device IRQ is ever lost, while keeping the
     * per-context-switch scheduler hot path free of the lock.
     */
    net_stack_receive_progress(CONFIG_NET_RX_IRQ_BUDGET);
}

void kernel_progress_run_bottom_halves(void) {
    if (__atomic_load_n(&g_progress_pending, __ATOMIC_ACQUIRE) &
        KERNEL_PROGRESS_PENDING_DEVICE)
        kernel_progress_devices();
    /* Free software queue credit before polling the device, then deliver
     * newly staged protocol events before the scheduler chooses a waiter.
     * Each pass is bounded; idle must not sleep with unfinished network work. */
    net_stack_protocol_progress(CONFIG_NET_RX_IRQ_BUDGET);
    kernel_progress_net_rx();
    net_stack_protocol_progress(CONFIG_NET_RX_IRQ_BUDGET);
}

int kernel_progress_work_pending(void) {
    return net_stack_work_pending();
}
