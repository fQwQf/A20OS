#ifndef _CORE_PROGRESS_H
#define _CORE_PROGRESS_H

#include "core/defs.h"

/*
 * KERNEL_PROGRESS_SERVICE_CONTRACT:
 * - Scheduler and idle code may request nonblocking kernel progress through this
 *   service, but they must not know which block or network driver implements it.
 * - Service callbacks must be nonblocking, must not sleep, and must not acquire
 *   locks in an order that violates the global lock order table.
 * - Until IRQ/bottom-half workers own all completions, this service is the
 *   single compatibility bridge for completion-polled devices and no-thread
 *   network progress.
 */
typedef enum kernel_progress_reason {
    KERNEL_PROGRESS_SCHED = 0,
    KERNEL_PROGRESS_IDLE,
    KERNEL_PROGRESS_IO_WAIT,
    KERNEL_PROGRESS_NET_WAIT,
} kernel_progress_reason_t;

/* Bits a producer ORs into the bridge's pending word from whatever context
 * creates work for it.  KERNEL_PROGRESS_PENDING_DEVICE covers the block-device
 * completion walk; OR-ing is idempotent, so producers may race each other and
 * race the consumer. */
#define KERNEL_PROGRESS_PENDING_DEVICE (1u << 0)

void kernel_progress_note_pending(uint32_t bits);
void kernel_progress_poll(kernel_progress_reason_t reason);
void kernel_progress_timer_tick(void);
void kernel_progress_run_bottom_halves(void);

#endif /* _CORE_PROGRESS_H */
