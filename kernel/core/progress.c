#include "core/progress.h"

#include "drivers/block/virtio_blk.h"
#include "net/lwip_stack.h"

/*
 * IO_PROGRESS_SERVICE:
 * - virtio block completion polling remains here until block IRQ/bottom-half
 *   completion queues wake waiters without scheduler help.
 * - lwIP NO_SYS polling remains here until a timer-driven or worker-driven
 *   network service owns sys_check_timeouts(), RX polling, and TX cleanup.
 */
void kernel_progress_poll(kernel_progress_reason_t reason)
{
    (void)reason;
    virtio_blk_poll_all();
    a20_lwip_poll();
}
