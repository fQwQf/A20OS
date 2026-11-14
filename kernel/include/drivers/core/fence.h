/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_FENCE_H
#define A20_DRIVER_FENCE_H

#include "core/types.h"

struct driver_fence;
struct driver_fence_watch;

/* One-shot native completion, independent of Linux sync_file/syncobj UAPI.
 * Creation/last put are task-context operations outside spinlocks. Every
 * waiter, producer and queued request must own a reference while accessing
 * the object. A producer retains its reference until its final signal call
 * returns. A fence does not own or stop DMA; stop hardware BEFORE signalling
 * an error that permits consumers to release request buffers. */
struct driver_fence *driver_fence_create(void);
void driver_fence_get(struct driver_fence *fence);
void driver_fence_put(struct driver_fence *fence);
/* IRQ-safe and non-allocating. error=0 succeeds, negative errno fails.
 * First result wins; subsequent signals return -EALREADY. */
int driver_fence_signal(struct driver_fence *fence, int error);
/* 0=pending, 1=success, negative errno=terminal failure. */
int driver_fence_status(struct driver_fence *fence);
/* Task context, caller holds a fence reference. Register one IRQ-safe,
 * nonblocking notification on a pending fence. Callback runs once with
 * status=1 or a negative errno, outside the fence lock, in the signalling
 * producer's context; it may run before create returns. Initialize context
 * before registration. Use it to schedule work, never to wait or free DMA.
 * A terminal fence returns -EALREADY without invoking/installing a callback;
 * its immutable result can then be read with status(). An initially terminal
 * fence requires no allocation; publication rechecks a completion racing
 * allocation. Allocation failure is
 * -ENOMEM; invalid arguments are -EINVAL. All errors clear a valid out.
 * Success transfers one watch owner to the caller. The watch retains its
 * fence, but NOT callback code/context. Keep both alive until removal succeeds.
 * The watch must be removed even after notification; signal never frees it. */
int driver_fence_watch_create(struct driver_fence *, void (*notify)(void *, int),
                               void *context, struct driver_fence_watch **out);
/* Task context, single owner: serialize calls on *watch. Does not wait.
 * Success prevents a pending callback or confirms it has returned, consumes
 * *watch to NULL and drops its fence reference outside the lock. Running
 * callback -> -EBUSY, preserving pointer/fence/code/context for explicit retry.
 * A visible terminal fence result alone does not prove callback completion.
 * A NULL argument -> -EINVAL; an already NULL watch succeeds. Cancellation
 * affects only this notification, never the fence result, work or hardware. */
int driver_fence_watch_try_remove(struct driver_fence_watch **watch);
/* Task context; no object/spinlock held. deadline is an absolute native
 * clock_get_ticks() value, 0 means no deadline. Returns 0 on success, the
 * producer's negative errno on failure, -ETIMEDOUT on wait expiry, -EINTR on
 * interruption, or -EAGAIN if no park token is available. Waiting never
 * cancels/signals a fence; timeout does not authorize freeing DMA storage.
 * A terminal result visible at the final check takes precedence over timeout.
 * The function takes an additional reference for the duration of the wait. */
int driver_fence_wait(struct driver_fence *fence, uint64_t deadline,
                       bool interruptible);

#endif
