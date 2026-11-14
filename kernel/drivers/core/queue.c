/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/queue.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/dma_buffer.h"
#include "drivers/core/fence.h"
#include "drivers/core/workqueue.h"
#include "mm/vmo.h"
#include "mm/slab.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "core/string.h"

enum request_state { REQUEST_NEW, REQUEST_PENDING, REQUEST_PREPARING,
                     REQUEST_ISSUED, REQUEST_DONE, REQUEST_TERMINAL };
struct driver_request {
    refcount_t references;
    struct driver_queue *queue;
    struct driver_fence *fence, *dependency;
    struct driver_fence_watch *dependency_watch;
    struct driver_request *previous, *next; /* accepted list */
    struct driver_request *ready_previous, *ready_next; /* pending FIFO */
    struct driver_request *done_next;
    void *private;
    uint64_t bytes;
    size_t command_size, memory_count;
    enum request_state state; /* queue lock */
    int error;
    bool active;
    struct driver_request_memory memory[]; /* followed by copied command */
};
struct driver_queue {
    refcount_t references;
    spinlock_t lock;
    struct driver_queue_limits limits;
    struct driver_queue_ops ops;
    void *context;
    struct driver_work *work;
    struct driver_request *first, *last, *pending, *pending_tail, *done, *done_tail;
    uint32_t outstanding, active;
    uint64_t bytes;
    int last_error;
    bool busy, closing, stopped;
};

void driver_queue_get(struct driver_queue *q) { if (q) refcount_inc(&q->references); }
void driver_queue_put(struct driver_queue *q)
{
    if (q && refcount_dec_and_test(&q->references)) kfree(q);
}
void driver_request_get(struct driver_request *r) { if (r) refcount_inc(&r->references); }
void driver_request_put(struct driver_request *r)
{
    if (!r || !refcount_dec_and_test(&r->references)) return;
    for (size_t i = 0; i < r->memory_count; i++) {
        if (r->memory[i].type == DRIVER_REQUEST_VMO) vmo_release(r->memory[i].owner.vmo);
        else driver_dma_buffer_put(r->memory[i].owner.buffer);
    }
    driver_fence_put(r->dependency);
    driver_fence_put(r->fence);
    driver_queue_put(r->queue);
    kfree(r);
}
const void *driver_request_command(const struct driver_request *r, size_t *size)
{
    if (size) *size = r ? r->command_size : 0;
    return r ? (const void *)(r->memory + r->memory_count) : NULL;
}
const struct driver_request_memory *driver_request_memory(const struct driver_request *r, size_t *count)
{
    if (count) *count = r ? r->memory_count : 0;
    return r ? r->memory : NULL;
}
struct driver_fence *driver_request_fence(const struct driver_request *r) { return r ? r->fence : NULL; }
void *driver_request_private(const struct driver_request *r) { return r ? r->private : NULL; }
void driver_request_set_private(struct driver_request *r, void *p) { if (r) r->private = p; }

int driver_request_create(struct driver_queue *q, const struct driver_request_description *d,
                            struct driver_request **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!q || !d || (d->command_size && !d->command) ||
        d->command_size > q->limits.max_command || d->memory_count > 64 ||
        (d->memory_count && !d->memory)) return -EINVAL;
    uint64_t bytes = d->command_size;
    for (size_t i = 0; i < d->memory_count; i++) {
        const struct driver_request_memory *m = &d->memory[i];
        uint64_t extent;
        if (m->type == DRIVER_REQUEST_VMO && m->owner.vmo &&
            (m->owner.vmo->type == VMO_CONTIGUOUS || m->owner.vmo->type == VMO_RESIDENT))
            extent = m->owner.vmo->size; /* immutable factory extents */
        else if (m->type == DRIVER_REQUEST_DMA_BUFFER && m->owner.buffer)
            extent = driver_dma_buffer_size(m->owner.buffer);
        else return -EINVAL;
        if (!m->length || m->offset > extent || m->length > extent - m->offset ||
            m->length > (uint64_t)-1 - bytes) return -EINVAL;
        bytes += m->length;
    }
    if (bytes > q->limits.max_bytes) return -E2BIG;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    bool closed = q->closing;
    spin_unlock_irqrestore(&q->lock, flags);
    if (closed) return -ESHUTDOWN;
    struct driver_request *r = kcalloc(1, sizeof(*r) + d->memory_count * sizeof(*r->memory) + d->command_size);
    if (!r) return -ENOMEM;
    r->fence = driver_fence_create();
    if (!r->fence) { kfree(r); return -ENOMEM; }
    refcount_set(&r->references, 1);
    r->queue = q; driver_queue_get(q);
    r->bytes = bytes; r->command_size = d->command_size; r->memory_count = d->memory_count;
    if (d->command_size) memcpy((void *)driver_request_command(r, NULL), d->command, d->command_size);
    for (size_t i = 0; i < d->memory_count; i++) {
        r->memory[i] = d->memory[i];
        if (r->memory[i].type == DRIVER_REQUEST_VMO) vmo_ref(r->memory[i].owner.vmo);
        else driver_dma_buffer_get(r->memory[i].owner.buffer);
    }
    r->dependency = d->dependency; driver_fence_get(r->dependency);
    *out = r;
    return 0;
}
/* Called with the queue lock, never enters provider code. */
static void kick(struct driver_queue *q, uint64_t deadline)
{
    if (q->work && !q->closing) {
        if (deadline) (void)driver_work_schedule(q->work, deadline);
        else (void)driver_work_reschedule(q->work, 0);
    }
}
/* Accepted request owns the queue until this watch has been cancelled or
 * its callback has returned. Callback never touches request/provider data. */
static void dependency_ready(void *context, int status)
{
    (void)status;
    struct driver_queue *q = context;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    kick(q, 0);
    spin_unlock_irqrestore(&q->lock, flags);
}
static int retire_request(struct driver_queue *q, struct driver_request *r)
{
    int ret = driver_fence_watch_try_remove(&r->dependency_watch);
    if (ret) return ret;
    return q->ops.retire ? q->ops.retire(q->context, r) : 0;
}
static void pending_remove(struct driver_queue *q, struct driver_request *r)
{
    if (r->ready_previous) r->ready_previous->ready_next = r->ready_next;
    else q->pending = r->ready_next;
    if (r->ready_next) r->ready_next->ready_previous = r->ready_previous;
    else q->pending_tail = r->ready_previous;
    r->ready_previous = r->ready_next = NULL;
}
static void done_append(struct driver_queue *q, struct driver_request *r)
{
    r->state = REQUEST_DONE; r->done_next = NULL;
    if (q->done_tail) q->done_tail->done_next = r;
    else q->done = r;
    q->done_tail = r;
}
int driver_request_submit(struct driver_request *r)
{
    if (!r) return -EINVAL;
    struct driver_queue *q = r->queue;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    int ret = r->state != REQUEST_NEW ? -EALREADY : q->closing ? -ESHUTDOWN :
        q->outstanding == q->limits.depth || r->bytes > q->limits.max_bytes - q->bytes ? -EAGAIN : 0;
    if (!ret) {
        driver_request_get(r);
        r->state = REQUEST_PENDING;
        r->previous = q->last;
        if (q->last) q->last->next = r;
        else q->first = r;
        q->last = r;
        r->ready_previous = q->pending_tail;
        if (q->pending_tail) q->pending_tail->ready_next = r;
        else q->pending = r;
        q->pending_tail = r;
        q->outstanding++; q->bytes += r->bytes;
        kick(q, 0);
    }
    spin_unlock_irqrestore(&q->lock, flags);
    return ret;
}
int driver_request_complete(struct driver_request *r, int error)
{
    if (!r || error > 0) return -EINVAL;
    struct driver_queue *q = r->queue;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    int ret = r->state == REQUEST_ISSUED ? 0 :
        r->state >= REQUEST_DONE ? -EALREADY : -EINVAL;
    if (!ret) { r->error = error; done_append(q, r); kick(q, 0); }
    spin_unlock_irqrestore(&q->lock, flags);
    return ret;
}
int driver_request_cancel(struct driver_request *r)
{
    if (!r) return -EINVAL;
    struct driver_queue *q = r->queue;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    int ret = r->state == REQUEST_PENDING ? 0 : r->state == REQUEST_NEW ? -ENOENT :
              r->state >= REQUEST_DONE ? -EALREADY : -EBUSY;
    if (!ret) { pending_remove(q, r); r->error = -ECANCELED; done_append(q, r); kick(q, 0); }
    spin_unlock_irqrestore(&q->lock, flags);
    return ret;
}
static void accepted_remove(struct driver_queue *q, struct driver_request *r)
{
    if (r->previous) r->previous->next = r->next;
    else q->first = r->next;
    if (r->next) r->next->previous = r->previous;
    else q->last = r->previous;
    q->outstanding--; q->bytes -= r->bytes;
    if (r->active) { q->active--; r->active = false; }
    r->state = REQUEST_TERMINAL;
}
/* No locks or provider dispatch lease: fence callbacks may reenter the queue. */
static void publish_finished(struct driver_request *list)
{
    while (list) {
        struct driver_request *r = list; list = r->done_next;
        (void)driver_fence_signal(r->fence, r->error);
        driver_request_put(r); /* accepted owner, may finalize native backing */
    }
}
int driver_queue_run(struct driver_queue *q, unsigned budget)
{
    if (!q || !budget || budget > 4096) return -EINVAL;
    struct driver_request *finished = NULL;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    if (q->busy) { spin_unlock_irqrestore(&q->lock, flags); return -EBUSY; }
    q->busy = true;
    unsigned processed = 0;
    int failure = 0;
    bool retry = false, dependency_wait = false;
    struct driver_request *failed_retire = NULL;
    while (processed < budget) {
        struct driver_request *r = q->done;
        if (r) {
            if (r == failed_retire) break;
            q->done = r->done_next;
            if (!q->done) q->done_tail = NULL;
            spin_unlock_irqrestore(&q->lock, flags);
            int ret = retire_request(q, r);
            flags = spin_lock_irqsave(&q->lock);
            if (ret) {
                if (!failure) failure = ret < 0 ? ret : -EIO;
                done_append(q, r); retry = true;
                if (!failed_retire) failed_retire = r;
                processed++;
                continue;
            }
            r->private = NULL;
            accepted_remove(q, r);
            r->done_next = finished; finished = r;
            processed++;
            continue;
        }
        r = q->pending;
        if (q->closing || !r || q->active == q->limits.max_active) break;
        int dependency = r->dependency ? driver_fence_status(r->dependency) : 1;
        if (!dependency) {
            if (q->work && !r->dependency_watch) {
                spin_unlock_irqrestore(&q->lock, flags);
                int ret = driver_fence_watch_create(r->dependency, dependency_ready,
                                                      q, &r->dependency_watch);
                flags = spin_lock_irqsave(&q->lock);
                /* Cancellation/completion can race watch creation. The
                 * accepted owner and dispatch lease preserve both objects. */
                if (ret == -EALREADY || r->state != REQUEST_PENDING) continue;
                if (ret) { failure = ret; retry = true; break; }
            }
            dependency_wait = true;
            break;
        }
        pending_remove(q, r);
        if (dependency < 0) { r->error = dependency; done_append(q, r); processed++; continue; }
        r->state = REQUEST_PREPARING; r->active = true; q->active++;
        spin_unlock_irqrestore(&q->lock, flags);
        int ret = q->ops.prepare ? q->ops.prepare(q->context, r) : 0;
        flags = spin_lock_irqsave(&q->lock);
        if (ret == -EAGAIN && !r->private) {
            r->state = REQUEST_PENDING; r->active = false; q->active--;
            r->ready_next = q->pending;
            if (q->pending) q->pending->ready_previous = r;
            else q->pending_tail = r;
            q->pending = r; retry = true; break;
        }
        if (ret) { r->error = ret < 0 ? ret : -EIO; done_append(q, r); }
        else if (q->closing) { r->error = -ESHUTDOWN; done_append(q, r); }
        else {
            r->state = REQUEST_ISSUED;
            spin_unlock_irqrestore(&q->lock, flags);
            q->ops.issue(q->context, r);
            flags = spin_lock_irqsave(&q->lock);
        }
        processed++;
    }
    if (failure) q->last_error = failure;
    if (q->done || (!dependency_wait && !q->closing && q->pending && q->active < q->limits.max_active))
        kick(q, retry ? clock_get_ticks() + 1 : 0);
    q->busy = false;
    spin_unlock_irqrestore(&q->lock, flags);
    publish_finished(finished);
    return failure ? failure : (int)processed;
}
static void queue_work(void *context) { (void)driver_queue_run(context, 64); }
int driver_queue_start(struct driver_queue *q, struct driver_workqueue *worker)
{
    if (!q || !worker) return -EINVAL;
    /* Owner serializes start with teardown. Do not allocate under a spinlock. */
    uint64_t flags = spin_lock_irqsave(&q->lock);
    int ret = q->closing ? -ESHUTDOWN : q->work ? -EALREADY : 0;
    spin_unlock_irqrestore(&q->lock, flags);
    if (ret) return ret;
    struct driver_work *work = driver_work_create(worker, queue_work, q);
    if (!work) return -ENOMEM;
    flags = spin_lock_irqsave(&q->lock);
    q->work = work; kick(q, 0);
    spin_unlock_irqrestore(&q->lock, flags);
    return 0;
}
int driver_queue_stop(struct driver_queue *q)
{
    if (!q) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    q->closing = true;
    if (q->busy) { spin_unlock_irqrestore(&q->lock, flags); return -EBUSY; }
    q->busy = true;
    spin_unlock_irqrestore(&q->lock, flags);
    int ret = q->work ? driver_work_cancel_sync(q->work, 0) : 0;
    if (!ret && !q->stopped) ret = q->ops.stop(q->context);
    struct driver_request *finished = NULL;
    flags = spin_lock_irqsave(&q->lock);
    if (ret) { ret = ret < 0 ? ret : -EIO; goto done; }
    q->stopped = true;
    /* Successful stop is the provider's proof that these requests can no
     * longer be touched. Preserve already observed completion errors. */
    q->pending = q->pending_tail = q->done = q->done_tail = NULL;
    for (struct driver_request *r = q->first; r;) {
        struct driver_request *next = r->next;
        if (r->state != REQUEST_DONE) r->error = -ESHUTDOWN;
        r->state = REQUEST_DONE;
        spin_unlock_irqrestore(&q->lock, flags);
        int cleanup = retire_request(q, r);
        flags = spin_lock_irqsave(&q->lock);
        if (cleanup) {
            if (!ret) ret = cleanup < 0 ? cleanup : -EIO;
            done_append(q, r);
        } else {
            r->private = NULL; accepted_remove(q, r);
            r->done_next = finished; finished = r;
        }
        r = next;
    }
    if (!ret) {
        /* Remaining request/queue/fence owners can outlive module code. */
        q->ops = (struct driver_queue_ops){0}; q->context = NULL;
    }
done:
    if (ret) q->last_error = ret;
    q->busy = false;
    spin_unlock_irqrestore(&q->lock, flags);
    publish_finished(finished);
    return ret;
}
void driver_queue_status(struct driver_queue *q, struct driver_queue_status *s)
{
    if (!s) return;
    *s = (struct driver_queue_status){0};
    if (!q) return;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    *s = (struct driver_queue_status){.outstanding = q->outstanding, .active = q->active,
        .bytes = q->bytes, .last_error = q->last_error, .closing = q->closing, .stopped = q->stopped};
    spin_unlock_irqrestore(&q->lock, flags);
}
static int managed_stop(void *q) { return driver_queue_stop(q); }
static void managed_release(void *context)
{
    struct driver_queue *q = context;
    if (q->work) { driver_work_put(q->work); q->work = NULL; }
    driver_queue_put(q);
}
int driver_queue_create(struct device *dev, const struct driver_queue_limits *limits,
                          const struct driver_queue_ops *ops, void *context,
                          struct driver_queue **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dev || !limits || !ops || ops->size < sizeof(*ops) ||
        !ops->issue || !ops->stop || !limits->depth ||
        !limits->max_active || limits->max_active > limits->depth ||
        limits->max_command > 4096 || !limits->max_bytes) return -EINVAL;
    int ret = device_probe_use_remove(dev);
    if (ret) return ret;
    struct driver_queue *q = kcalloc(1, sizeof(*q));
    if (!q) return -ENOMEM;
    refcount_set(&q->references, 2); spin_init(&q->lock);
    q->limits = *limits; q->ops = *ops; q->context = context;
    ret = device_add_execution_action(dev, managed_stop, managed_release, q);
    if (ret) { kfree(q); return ret; }
    *out = q;
    return 0;
}
#endif
