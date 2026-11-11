/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_FENCE_H
#define A20_DRIVER_FENCE_H

#include "core/types.h"

struct driver_fence;

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
