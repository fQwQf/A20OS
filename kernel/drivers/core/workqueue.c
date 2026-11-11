/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/workqueue.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "mm/slab.h"
#include "proc/proc.h"

struct driver_workqueue {
    refcount_t references;
    spinlock_t lock;
    wait_queue_t ready;
    wait_queue_t changed;
    struct driver_work *head;
    struct driver_work *running;
    task_t *worker;
    bool stopping;
    bool exited;
    bool bootstrap_ready;
    struct driver_workqueue *bootstrap_next;
};

struct driver_work {
    refcount_t references;
    struct driver_workqueue *queue;
    void (*callback)(void *);
    void *context;
    struct driver_work *next;
    uint64_t deadline;
    unsigned cancelers;
    bool pending;
    bool running;
};

/* proc_alloc_detached has no entry argument. Workers only claim committed
 * entries; a failed allocation removes its uncommitted queue. Never hold a
 * lock across task allocation: reclaim may need an older queue's worker. */
static spinlock_t bootstrap_lock = SPINLOCK_INIT;
static struct driver_workqueue *bootstrap_queues;

static void queue_put(struct driver_workqueue *queue)
{
    if (refcount_dec_and_test(&queue->references))
        kfree(queue);
}

void driver_work_get(struct driver_work *work)
{
    if (work)
        refcount_inc(&work->references);
}

void driver_work_put(struct driver_work *work)
{
    if (work && refcount_dec_and_test(&work->references)) {
        queue_put(work->queue);
        kfree(work);
    }
}

static void notify_change(struct driver_workqueue *queue)
{
    wait_queue_wake_all(&queue->changed, 0, PROC_WAKE_EVENT);
}

static bool ready_locked(struct driver_workqueue *queue)
{
    return queue->head && (!queue->head->deadline ||
                           clock_get_ticks() >= queue->head->deadline);
}

static void worker_main(void)
{
    struct driver_workqueue *queue = NULL;
    while (!queue) {
        uint64_t bootstrap_flags = spin_lock_irqsave(&bootstrap_lock);
        struct driver_workqueue **at = &bootstrap_queues;
        while (*at && !(*at)->bootstrap_ready)
            at = &(*at)->bootstrap_next;
        if (*at) {
            queue = *at;
            *at = queue->bootstrap_next;
            queue->bootstrap_next = NULL;
        }
        spin_unlock_irqrestore(&bootstrap_lock, bootstrap_flags);
        if (!queue)
            proc_yield(); /* Creator has not yet committed its successful spawn. */
    }
    uint64_t flags = spin_lock_irqsave(&queue->lock);
    queue->worker = proc_current();
    spin_unlock_irqrestore(&queue->lock, flags);

    for (;;) {
        flags = spin_lock_irqsave(&queue->lock);
        if (queue->stopping) {
            queue->worker = NULL;
            queue->exited = true;
            spin_unlock_irqrestore(&queue->lock, flags);
            notify_change(queue);
            queue_put(queue); /* The final access by this detached worker. */
            return;
        }
        if (ready_locked(queue)) {
            struct driver_work *work = queue->head;
            queue->head = work->next;
            work->next = NULL;
            work->pending = false;
            work->running = true;
            queue->running = work;
            spin_unlock_irqrestore(&queue->lock, flags);
            work->callback(work->context);
            flags = spin_lock_irqsave(&queue->lock);
            work->running = false;
            queue->running = NULL;
            spin_unlock_irqrestore(&queue->lock, flags);
            notify_change(queue);
            driver_work_put(work); /* Pending reference transferred to execution. */
            continue;
        }
        uint64_t deadline = queue->head ? queue->head->deadline : 0;
        spin_unlock_irqrestore(&queue->lock, flags);

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE,
                                                    deadline);
        if (!token.task) {
            proc_yield();
            continue;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&queue->lock);
        bool unchanged = (queue->head ? queue->head->deadline : 0) == deadline;
        bool linked = !queue->stopping && !ready_locked(queue) && unchanged &&
            wait_queue_link(&queue->ready, &entry, token, 0);
        spin_unlock_irqrestore(&queue->lock, flags);
        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&queue->ready, &entry);
        proc_park_finish(token);
    }
}

struct driver_workqueue *driver_workqueue_create(void)
{
    if (!proc_current())
        return NULL;
    struct driver_workqueue *queue = kcalloc(1, sizeof(*queue));
    if (!queue)
        return NULL;
    refcount_set(&queue->references, 2); /* Caller and detached worker. */
    spin_init(&queue->lock);
    wait_queue_init(&queue->ready);
    wait_queue_init(&queue->changed);
    uint64_t flags = spin_lock_irqsave(&bootstrap_lock);
    queue->bootstrap_next = bootstrap_queues;
    bootstrap_queues = queue;
    spin_unlock_irqrestore(&bootstrap_lock, flags);
    int pid = proc_alloc_detached(worker_main);
    flags = spin_lock_irqsave(&bootstrap_lock);
    if (pid < 0) {
        struct driver_workqueue **at = &bootstrap_queues;
        while (*at != queue)
            at = &(*at)->bootstrap_next;
        *at = queue->bootstrap_next;
    } else {
        queue->bootstrap_ready = true;
    }
    spin_unlock_irqrestore(&bootstrap_lock, flags);
    if (pid < 0) {
        kfree(queue);
        return NULL;
    }
    return queue;
}

struct driver_work *driver_work_create(struct driver_workqueue *queue,
                                        void (*callback)(void *), void *context)
{
    if (!queue || !callback)
        return NULL;
    struct driver_work *work = kcalloc(1, sizeof(*work));
    if (!work)
        return NULL;
    refcount_set(&work->references, 1);
    work->queue = queue;
    work->callback = callback;
    work->context = context;
    uint64_t flags = spin_lock_irqsave(&queue->lock);
    if (queue->stopping) {
        spin_unlock_irqrestore(&queue->lock, flags);
        kfree(work);
        return NULL;
    }
    refcount_inc(&queue->references);
    spin_unlock_irqrestore(&queue->lock, flags);
    return work;
}

static int schedule_work(struct driver_work *work, uint64_t deadline, bool replace)
{
    if (!work)
        return -EINVAL;
    struct driver_workqueue *queue = work->queue;
    uint64_t flags = spin_lock_irqsave(&queue->lock);
    int rc;
    bool wake = false;
    if (queue->stopping)
        rc = -ESHUTDOWN;
    else if (work->cancelers)
        rc = -EBUSY;
    else if (work->pending && !replace)
        rc = 0;
    else {
        bool was_pending = work->pending;
        if (was_pending) {
            struct driver_work **old = &queue->head;
            while (*old != work)
                old = &(*old)->next;
            *old = work->next;
        }
        work->deadline = deadline;
        struct driver_work **at = &queue->head;
        while (*at && (*at)->deadline <= deadline)
            at = &(*at)->next;
        work->next = *at;
        *at = work;
        work->pending = true;
        if (!was_pending)
            driver_work_get(work);
        rc = was_pending ? 0 : 1;
        wake = true;
    }
    spin_unlock_irqrestore(&queue->lock, flags);
    if (wake)
        wait_queue_wake_all(&queue->ready, 0, PROC_WAKE_EVENT);
    return rc;
}

int driver_work_schedule(struct driver_work *work, uint64_t deadline)
{
    return schedule_work(work, deadline, false);
}

int driver_work_reschedule(struct driver_work *work, uint64_t deadline)
{
    return schedule_work(work, deadline, true);
}

enum wait_condition { WORK_IDLE, QUEUE_IDLE, QUEUE_EXITED };

static bool done_locked(struct driver_workqueue *queue, struct driver_work *work,
                          enum wait_condition condition)
{
    if (condition == WORK_IDLE)
        return !work->running;
    if (condition == QUEUE_EXITED)
        return queue->exited;
    return !queue->head && !queue->running;
}

static int wait_until(struct driver_workqueue *queue, struct driver_work *work,
                       enum wait_condition condition, uint64_t deadline)
{
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&queue->lock);
        bool done = done_locked(queue, work, condition);
        spin_unlock_irqrestore(&queue->lock, flags);
        if (done)
            return 0;
        if (deadline && clock_get_ticks() >= deadline) {
            flags = spin_lock_irqsave(&queue->lock);
            done = done_locked(queue, work, condition);
            spin_unlock_irqrestore(&queue->lock, flags);
            return done ? 0 : -ETIMEDOUT;
        }
        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE,
                                                    deadline);
        if (!token.task) {
            proc_yield();
            continue;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&queue->lock);
        bool linked = !done_locked(queue, work, condition) &&
            wait_queue_link(&queue->changed, &entry, token, 0);
        spin_unlock_irqrestore(&queue->lock, flags);
        proc_wake_reason_t reason = PROC_WAKE_NONE;
        if (linked)
            reason = proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&queue->changed, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason) ||
            reason == PROC_WAKE_CANCEL || reason == PROC_WAKE_EXIT) {
            flags = spin_lock_irqsave(&queue->lock);
            done = done_locked(queue, work, condition);
            spin_unlock_irqrestore(&queue->lock, flags);
            return done ? 0 : -EINTR;
        }
    }
}

int driver_work_cancel_sync(struct driver_work *work, uint64_t deadline)
{
    if (!work)
        return -EINVAL;
    if (!proc_current())
        return -EAGAIN;
    struct driver_workqueue *queue = work->queue;
    uint64_t flags = spin_lock_irqsave(&queue->lock);
    if (work->running && queue->worker == proc_current()) {
        spin_unlock_irqrestore(&queue->lock, flags);
        return -EDEADLK;
    }
    work->cancelers++;
    bool removed = work->pending;
    if (removed) {
        struct driver_work **at = &queue->head;
        while (*at != work)
            at = &(*at)->next;
        *at = work->next;
        work->next = NULL;
        work->pending = false;
    }
    spin_unlock_irqrestore(&queue->lock, flags);
    if (removed) {
        driver_work_put(work);
        wait_queue_wake_all(&queue->ready, 0, PROC_WAKE_EVENT);
        notify_change(queue);
    }
    int rc = wait_until(queue, work, WORK_IDLE, deadline);
    flags = spin_lock_irqsave(&queue->lock);
    work->cancelers--;
    spin_unlock_irqrestore(&queue->lock, flags);
    return rc;
}

int driver_workqueue_flush(struct driver_workqueue *queue, uint64_t deadline)
{
    if (!queue)
        return -EINVAL;
    if (!proc_current())
        return -EAGAIN;
    uint64_t flags = spin_lock_irqsave(&queue->lock);
    bool self = queue->worker == proc_current();
    spin_unlock_irqrestore(&queue->lock, flags);
    return self ? -EDEADLK : wait_until(queue, NULL, QUEUE_IDLE, deadline);
}

int driver_workqueue_destroy(struct driver_workqueue **pointer, uint64_t deadline)
{
    if (!pointer || !*pointer)
        return -EINVAL;
    if (!proc_current())
        return -EAGAIN;
    struct driver_workqueue *queue = *pointer;
    uint64_t flags = spin_lock_irqsave(&queue->lock);
    if (queue->worker == proc_current()) {
        spin_unlock_irqrestore(&queue->lock, flags);
        return -EDEADLK;
    }
    queue->stopping = true;
    struct driver_work *pending = queue->head;
    queue->head = NULL;
    for (struct driver_work *work = pending; work; work = work->next)
        work->pending = false;
    spin_unlock_irqrestore(&queue->lock, flags);
    while (pending) {
        struct driver_work *next = pending->next;
        pending->next = NULL;
        driver_work_put(pending);
        pending = next;
    }
    wait_queue_wake_all(&queue->ready, 0, PROC_WAKE_EVENT);
    notify_change(queue);
    int rc = wait_until(queue, NULL, QUEUE_EXITED, deadline);
    if (!rc) {
        *pointer = NULL;
        queue_put(queue);
    }
    return rc;
}
#endif
