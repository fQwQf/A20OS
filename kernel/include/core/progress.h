#ifndef _CORE_PROGRESS_H
#define _CORE_PROGRESS_H

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

void kernel_progress_poll(kernel_progress_reason_t reason);

#endif /* _CORE_PROGRESS_H */
