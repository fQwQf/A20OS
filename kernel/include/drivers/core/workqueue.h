/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_WORKQUEUE_H
#define A20_DRIVER_WORKQUEUE_H

#include "core/types.h"

struct driver_workqueue;
struct driver_work;

/* Hosted kernel, task context after scheduler initialization. One detached
 * kernel worker per queue, callbacks serialized within that queue. The worker
 * inherits no user files/signals. Returns NULL on allocation/start failure.
 * The caller owns the queue: serialize work creation/flush against destroy,
 * and serialize destroy retries. Producers use referenced work objects. */
struct driver_workqueue *driver_workqueue_create(void);
/* Work is permanently bound to one queue. Creation/last put are task-context
 * operations. A work object retains its queue even after queue destruction;
 * subsequent submissions then fail with -ESHUTDOWN. Callback code/context
 * must remain live until cancel_sync succeeds or the queue is destroyed.
 * Callers/producers must own a work reference throughout each operation. */
struct driver_work *driver_work_create(struct driver_workqueue *queue,
                                        void (*callback)(void *), void *context);
void driver_work_get(struct driver_work *work);
void driver_work_put(struct driver_work *work);
/* IRQ-safe, non-allocating. Absolute clock_get_ticks() deadline; 0 means now.
 * Returns 1 when queued, 0 if already pending (keeps original deadline),
 * -EBUSY while cancellation is in progress, -ESHUTDOWN after destroy starts.
 * A running callback may queue one further execution of itself. */
int driver_work_schedule(struct driver_work *work, uint64_t deadline);
/* IRQ-safe timer rearm: replace an existing pending deadline and reposition
 * work in the queue. Returns 0 when replaced, 1 when newly queued; same error
 * rules as schedule. Wakes an idle worker so an earlier deadline takes effect. */
int driver_work_reschedule(struct driver_work *work, uint64_t deadline);
/* Task context, no locks held that the callback may need. Remove pending
 * execution, suppress submissions during cancellation, then wait for a
 * running callback to return. Success does not prevent later resubmission:
 * stop other producers before freeing callback context. Self-cancel of a
 * running callback returns -EDEADLK. */
int driver_work_cancel_sync(struct driver_work *work, uint64_t deadline);
/* Wait for all pending (including delayed) and running work. Producers may
 * submit after the final idle check: flush is not a submission barrier. */
int driver_workqueue_flush(struct driver_workqueue *queue, uint64_t deadline);
/* Permanently close submissions, cancel pending work, and wait for the worker
 * to exit. Success consumes *queue and sets it to NULL; work references may
 * keep its offline shell alive. Timeout retains *queue for a later retry.
 * Queue/callback storage must not be freed after a failed destroy. */
int driver_workqueue_destroy(struct driver_workqueue **queue, uint64_t deadline);
/* Blocking operations above use absolute native ticks; deadline=0 means no
 * deadline. They return -ETIMEDOUT on expiry, -EINTR on task interruption,
 * -EAGAIN outside task context,
 * and -EDEADLK when the queue's own worker would have to wait for itself.
 * An observed completed condition wins over an expired deadline. */

#endif
