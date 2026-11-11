/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/fence.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "mm/slab.h"

struct driver_fence {
    refcount_t references;
    spinlock_t lock;
    int status;
    wait_queue_t waiters;
};

struct driver_fence *driver_fence_create(void)
{
    struct driver_fence *fence = kmalloc(sizeof(*fence));
    if (!fence)
        return NULL;
    refcount_set(&fence->references, 1);
    spin_init(&fence->lock);
    fence->status = 0;
    wait_queue_init(&fence->waiters);
    return fence;
}

void driver_fence_get(struct driver_fence *fence)
{
    if (fence)
        refcount_inc(&fence->references);
}

void driver_fence_put(struct driver_fence *fence)
{
    if (fence && refcount_dec_and_test(&fence->references))
        kfree(fence);
}

int driver_fence_status(struct driver_fence *fence)
{
    if (!fence)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&fence->lock);
    int status = fence->status;
    spin_unlock_irqrestore(&fence->lock, flags);
    return status;
}

int driver_fence_signal(struct driver_fence *fence, int error)
{
    if (!fence || error > 0)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&fence->lock);
    if (fence->status) {
        spin_unlock_irqrestore(&fence->lock, flags);
        return -EALREADY;
    }
    fence->status = error ? error : 1;
    spin_unlock_irqrestore(&fence->lock, flags);
    /* No queue/scheduler operations under the fence lock. Waiters link while
     * holding that same lock and cannot link after observing a terminal state. */
    wait_queue_wake_all(&fence->waiters, 0, PROC_WAKE_EVENT);
    return 0;
}

int driver_fence_wait(struct driver_fence *fence, uint64_t deadline,
                       bool interruptible)
{
    if (!fence)
        return -EINVAL;
    driver_fence_get(fence);
    int result;
    for (;;) {
        int status = driver_fence_status(fence);
        if (status) {
            result = status < 0 ? status : 0;
            break;
        }
        if (deadline && clock_get_ticks() >= deadline) {
            status = driver_fence_status(fence);
            result = status ? (status < 0 ? status : 0) : -ETIMEDOUT;
            break;
        }
        proc_wait_token_t token = proc_park_prepare(interruptible ?
            PROC_WAIT_INTERRUPTIBLE : PROC_WAIT_UNINTERRUPTIBLE, deadline);
        if (!token.task) {
            status = driver_fence_status(fence);
            result = status ? (status < 0 ? status : 0) : -EAGAIN;
            break;
        }
        wait_queue_entry_t entry = {0};
        uint64_t flags = spin_lock_irqsave(&fence->lock);
        status = fence->status;
        bool linked = !status &&
            wait_queue_link(&fence->waiters, &entry, token, 0);
        spin_unlock_irqrestore(&fence->lock, flags);
        proc_wake_reason_t reason = PROC_WAKE_NONE;
        if (linked)
            reason = proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&fence->waiters, &entry);
        proc_park_finish(token);
        status = driver_fence_status(fence);
        if (status) {
            result = status < 0 ? status : 0;
            break;
        }
        if (!linked || reason == PROC_WAKE_TIMEOUT_CAPACITY) {
            result = -EAGAIN;
            break;
        }
        if (reason == PROC_WAKE_TIMEOUT) {
            result = -ETIMEDOUT;
            break;
        }
        if (proc_wake_reason_is_task_interrupt(reason) ||
            reason == PROC_WAKE_CANCEL || reason == PROC_WAKE_EXIT) {
            result = -EINTR;
            break;
        }
    }
    driver_fence_put(fence);
    return result;
}
#endif
