/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/fence.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "mm/slab.h"

enum watch_state { WATCH_PENDING, WATCH_RUNNING, WATCH_DONE };
struct driver_fence_watch {
    struct driver_fence *fence;
    struct driver_fence_watch *previous, *next;
    void (*notify)(void *, int);
    void *context;
    enum watch_state state; /* fence lock, including callback retirement */
};

struct driver_fence {
    refcount_t references;
    spinlock_t lock;
    int status;
    wait_queue_t waiters;
    struct driver_fence_watch *first, *last;
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
    fence->first = fence->last = NULL;
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

/* Fence lock held; constant-time unlink avoids walking other registrations
 * with interrupts disabled. Detached storage remains owned by its caller. */
static void unlink_watch(struct driver_fence_watch *watch)
{
    struct driver_fence *fence = watch->fence;
    if (watch->previous) watch->previous->next = watch->next;
    else fence->first = watch->next;
    if (watch->next) watch->next->previous = watch->previous;
    else fence->last = watch->previous;
    watch->previous = watch->next = NULL;
}

int driver_fence_watch_create(struct driver_fence *fence,
                               void (*notify)(void *, int), void *context,
                               struct driver_fence_watch **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!fence || !notify) return -EINVAL;
    /* Already satisfied dependencies must not need registration storage.
     * Completion after this check is still arbitrated at publication below. */
    if (driver_fence_status(fence)) return -EALREADY;
    struct driver_fence_watch *watch = kmalloc(sizeof(*watch));
    if (!watch) return -ENOMEM;
    watch->fence = fence;
    watch->notify = notify;
    watch->context = context;
    watch->state = WATCH_PENDING;
    watch->next = NULL;
    uint64_t flags = spin_lock_irqsave(&fence->lock);
    if (fence->status) {
        spin_unlock_irqrestore(&fence->lock, flags);
        kfree(watch);
        return -EALREADY;
    }
    driver_fence_get(fence);
    watch->previous = fence->last;
    if (fence->last) fence->last->next = watch;
    else fence->first = watch;
    fence->last = watch;
    *out = watch;
    spin_unlock_irqrestore(&fence->lock, flags);
    return 0;
}

int driver_fence_watch_try_remove(struct driver_fence_watch **pointer)
{
    if (!pointer) return -EINVAL;
    struct driver_fence_watch *watch = *pointer;
    if (!watch) return 0;
    struct driver_fence *fence = watch->fence;
    uint64_t flags = spin_lock_irqsave(&fence->lock);
    if (watch->state == WATCH_RUNNING) {
        spin_unlock_irqrestore(&fence->lock, flags);
        return -EBUSY;
    }
    if (watch->state == WATCH_PENDING) unlink_watch(watch);
    spin_unlock_irqrestore(&fence->lock, flags);
    *pointer = NULL;
    driver_fence_put(fence);
    kfree(watch);
    return 0;
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
    /* Select each notification under the lock; removal may cancel any still
     * pending entry. A selected watch cannot be freed until this producer
     * has returned from its callback and published WATCH_DONE. Producer and
     * watch references keep the fence alive; there is no IRQ allocation/put. */
    for (;;) {
        flags = spin_lock_irqsave(&fence->lock);
        struct driver_fence_watch *watch = fence->first;
        if (!watch) {
            spin_unlock_irqrestore(&fence->lock, flags);
            break;
        }
        unlink_watch(watch);
        watch->state = WATCH_RUNNING;
        spin_unlock_irqrestore(&fence->lock, flags);
        watch->notify(watch->context, error ? error : 1);
        flags = spin_lock_irqsave(&fence->lock);
        watch->state = WATCH_DONE;
        spin_unlock_irqrestore(&fence->lock, flags);
        /* Do not access watch again: its owner may now remove it. */
    }
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
