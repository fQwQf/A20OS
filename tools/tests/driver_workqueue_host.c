/* Production workqueue with pthread substitutes for kernel tasks/Park/Wake. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "drivers/core/workqueue.h"
#include "core/sync.h"
#include "proc/proc.h"

_Thread_local unsigned host_lock_depth;
static _Thread_local int no_task;
static unsigned allocations, parked;
static int allocation_failure, spawn_failure;
void *kcalloc(size_t n, size_t size)
{
    assert(!host_lock_depth);
    if (__atomic_exchange_n(&allocation_failure, 0, __ATOMIC_RELAXED)) return NULL;
    void *p = calloc(n, size); assert(p);
    __atomic_add_fetch(&allocations, 1, __ATOMIC_RELAXED); return p;
}
void kfree(void *p)
{
    assert(!host_lock_depth);
    if (p) { __atomic_sub_fetch(&allocations, 1, __ATOMIC_RELAXED); free(p); }
}
uint64_t clock_get_ticks(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
struct task_t {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint64_t sequence, deadline;
    int ready;
};
static _Thread_local struct task_t local_task;
static _Thread_local int task_initialized;
task_t *proc_current(void)
{
    if (no_task) return NULL;
    if (!task_initialized) {
        pthread_mutex_init(&local_task.lock, NULL);
        pthread_condattr_t attr; pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&local_task.condition, &attr);
        pthread_condattr_destroy(&attr); task_initialized = 1;
    }
    return &local_task;
}
void proc_yield(void) { assert(!host_lock_depth); sched_yield(); }
static pthread_t workers[64];
static unsigned worker_count;
static pthread_mutex_t workers_lock = PTHREAD_MUTEX_INITIALIZER;
static void *task_start(void *entry)
{ proc_current(); ((void (*)(void))entry)(); return NULL; }
int proc_alloc_detached(void (*entry)(void))
{
    assert(!host_lock_depth);
    if (spawn_failure) { spawn_failure = 0; return -EAGAIN; }
    pthread_mutex_lock(&workers_lock);
    assert(worker_count < 64);
    assert(!pthread_create(&workers[worker_count], NULL, task_start, (void *)entry));
    int pid = ++worker_count;
    pthread_mutex_unlock(&workers_lock); return pid;
}
static void join_workers(void)
{
    for (unsigned i = 0; i < worker_count; i++) pthread_join(workers[i], NULL);
    worker_count = 0;
    assert(!allocations && !parked);
}
void mutex_lock(mutex_t *m) { assert(!host_lock_depth); pthread_mutex_lock(&m->lock); }
void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(&m->lock); }
void wait_queue_init(wait_queue_t *q)
{ spin_init(&q->lock); q->head = NULL; q->generation = 0; }
bool wait_queue_link(wait_queue_t *q, wait_queue_entry_t *e,
                      proc_wait_token_t token, uintptr_t key)
{
    uint64_t flags = spin_lock_irqsave(&q->lock);
    e->task = token.task; e->next = q->head; e->key = key;
    e->linked = true; q->head = e;
    spin_unlock_irqrestore(&q->lock, flags); return true;
}
void wait_queue_unlink(wait_queue_t *q, wait_queue_entry_t *e)
{
    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t **p = &q->head;
    while (*p && *p != e) p = &(*p)->next;
    if (*p) { *p = e->next; e->linked = false; }
    spin_unlock_irqrestore(&q->lock, flags);
}
unsigned wait_queue_wake_all(wait_queue_t *q, uintptr_t key, proc_wake_reason_t reason)
{
    assert(!host_lock_depth && reason == PROC_WAKE_EVENT);
    unsigned count = 0;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    while (q->head) {
        wait_queue_entry_t *e = q->head;
        q->head = e->next; e->linked = false; assert(e->key == key);
        task_t *t = e->task;
        pthread_mutex_lock(&t->lock);
        t->ready = 1; pthread_cond_signal(&t->condition);
        pthread_mutex_unlock(&t->lock); count++;
    }
    spin_unlock_irqrestore(&q->lock, flags); return count;
}
proc_wait_token_t proc_park_prepare(proc_wait_mode_t mode, uint64_t deadline)
{
    assert(!host_lock_depth && mode == PROC_WAIT_UNINTERRUPTIBLE);
    task_t *t = proc_current(); assert(t);
    pthread_mutex_lock(&t->lock);
    t->ready = 0; t->sequence++; t->deadline = deadline;
    pthread_mutex_unlock(&t->lock);
    return (proc_wait_token_t){.task=t, .seq=t->sequence};
}
int proc_park_cancel(proc_wait_token_t token)
{ (void)token; assert(!host_lock_depth); return 0; }
void proc_park_finish(proc_wait_token_t token)
{ (void)token; assert(!host_lock_depth); }
proc_wake_reason_t proc_park_commit(proc_wait_token_t token)
{
    assert(!host_lock_depth);
    task_t *t = token.task;
    pthread_mutex_lock(&t->lock);
    __atomic_add_fetch(&parked, 1, __ATOMIC_RELEASE);
    while (!t->ready) {
        int rc;
        if (t->deadline) {
            struct timespec deadline = {t->deadline / 1000, (t->deadline % 1000) * 1000000};
            rc = pthread_cond_timedwait(&t->condition, &t->lock, &deadline);
        } else rc = pthread_cond_wait(&t->condition, &t->lock);
        if (rc == ETIMEDOUT && !t->ready) {
            __atomic_sub_fetch(&parked, 1, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&t->lock); return PROC_WAKE_TIMEOUT;
        }
    }
    __atomic_sub_fetch(&parked, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&t->lock); return PROC_WAKE_EVENT;
}

static void increment(void *p) { assert(!host_lock_depth); (*(unsigned *)p)++; }
struct gate {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int entered, released;
};
#define GATE_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0}
static void blocking(void *p)
{
    struct gate *gate = p; assert(!host_lock_depth);
    pthread_mutex_lock(&gate->lock); gate->entered++;
    pthread_cond_broadcast(&gate->changed);
    while (!gate->released) pthread_cond_wait(&gate->changed, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}
static void wait_entered(struct gate *gate)
{
    pthread_mutex_lock(&gate->lock);
    while (!gate->entered) pthread_cond_wait(&gate->changed, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}
static void release_gate(struct gate *gate)
{
    pthread_mutex_lock(&gate->lock); gate->released = 1;
    pthread_cond_broadcast(&gate->changed); pthread_mutex_unlock(&gate->lock);
}
static void ordered_and_delayed(void)
{
    struct driver_workqueue *q = driver_workqueue_create(); assert(q);
    unsigned early = 0, late = 0;
    struct driver_work *a = driver_work_create(q, increment, &early);
    struct driver_work *b = driver_work_create(q, increment, &late);
    assert(a && b);
    uint64_t due = clock_get_ticks() + 80;
    assert(driver_work_schedule(b, due) == 1);
    assert(driver_work_schedule(b, 0) == 0); /* Coalescing retains delay. */
    assert(driver_workqueue_flush(q, clock_get_ticks() + 10) == -ETIMEDOUT);
    assert(!late);
    assert(driver_work_schedule(a, 0) == 1);
    assert(!driver_workqueue_flush(q, clock_get_ticks() + 1000));
    assert(early == 1 && late == 1 && clock_get_ticks() >= due);
    assert(driver_work_schedule(b, clock_get_ticks() + 10000) == 1);
    due = clock_get_ticks() + 20;
    assert(driver_work_reschedule(b, due) == 0);
    assert(!driver_workqueue_flush(q, clock_get_ticks() + 1000));
    assert(late == 2 && clock_get_ticks() >= due);
    driver_work_put(a); driver_work_put(b);
    assert(!driver_workqueue_destroy(&q, 0) && !q); join_workers();
}
struct requeue { struct driver_workqueue *queue; struct driver_work *work; unsigned count; };
static void resubmit(void *p)
{
    struct requeue *r = p; r->count++;
    if (r->count == 1) assert(driver_work_schedule(r->work, 0) == 1);
    else {
        assert(driver_work_cancel_sync(r->work, 0) == -EDEADLK);
        assert(driver_workqueue_flush(r->queue, 0) == -EDEADLK);
        struct driver_workqueue *copy = r->queue;
        assert(driver_workqueue_destroy(&copy, 0) == -EDEADLK && copy == r->queue);
    }
}
static void self_resubmit(void)
{
    struct requeue r = {.queue=driver_workqueue_create()}; assert(r.queue);
    r.work = driver_work_create(r.queue, resubmit, &r); assert(r.work);
    assert(driver_work_schedule(r.work, 0) == 1);
    assert(!driver_workqueue_flush(r.queue, clock_get_ticks() + 1000) && r.count == 2);
    driver_work_put(r.work); assert(!driver_workqueue_destroy(&r.queue, 0)); join_workers();
}
struct cancel_arg { struct driver_work *work; int result; };
static void *cancel_thread(void *p)
{
    struct cancel_arg *arg = p;
    arg->result = driver_work_cancel_sync(arg->work, clock_get_ticks() + 1000); return NULL;
}
static void cancellation(void)
{
    struct driver_workqueue *q = driver_workqueue_create(); assert(q);
    struct gate gate = GATE_INIT;
    unsigned canceled = 0;
    struct driver_work *a = driver_work_create(q, blocking, &gate);
    struct driver_work *b = driver_work_create(q, increment, &canceled);
    assert(driver_work_schedule(a, 0) == 1); wait_entered(&gate);
    assert(driver_work_schedule(b, 0) == 1);
    assert(!driver_work_cancel_sync(b, 0) && !canceled);
    assert(driver_work_cancel_sync(a, clock_get_ticks() + 10) == -ETIMEDOUT);
    pthread_t first, second;
    struct cancel_arg one = {a, -1}, two = {a, -1};
    pthread_create(&first, NULL, cancel_thread, &one);
    pthread_create(&second, NULL, cancel_thread, &two);
    while (__atomic_load_n(&parked, __ATOMIC_ACQUIRE) < 2) sched_yield();
    assert(driver_work_schedule(a, 0) == -EBUSY);
    release_gate(&gate); pthread_join(first, NULL); pthread_join(second, NULL);
    assert(!one.result && !two.result);
    assert(driver_work_schedule(b, 0) == 1);
    assert(!driver_workqueue_flush(q, clock_get_ticks() + 1000) && canceled == 1);
    driver_work_put(a); driver_work_put(b);
    assert(!driver_workqueue_destroy(&q, 0)); join_workers();
}
static void shutdown_timeout(void)
{
    struct driver_workqueue *q = driver_workqueue_create(); assert(q);
    struct gate gate = GATE_INIT;
    unsigned canceled = 0;
    struct driver_work *a = driver_work_create(q, blocking, &gate);
    struct driver_work *b = driver_work_create(q, increment, &canceled);
    assert(driver_work_schedule(a, 0) == 1); wait_entered(&gate);
    assert(driver_work_schedule(b, 0) == 1);
    assert(driver_workqueue_destroy(&q, clock_get_ticks() + 10) == -ETIMEDOUT && q);
    assert(driver_work_schedule(b, 0) == -ESHUTDOWN && !canceled);
    assert(!driver_work_create(q, increment, &canceled));
    release_gate(&gate); assert(!driver_workqueue_destroy(&q, 0) && !q);
    assert(driver_work_schedule(a, 0) == -ESHUTDOWN); /* Offline queue retained by work. */
    driver_work_put(a); driver_work_put(b); join_workers();
}
static void failures_and_pending_reference(void)
{
    no_task = 1; assert(!driver_workqueue_create()); no_task = 0;
    allocation_failure = 1; assert(!driver_workqueue_create());
    spawn_failure = 1; assert(!driver_workqueue_create() && !allocations);
    struct driver_workqueue *q = driver_workqueue_create(); assert(q);
    unsigned count = 0;
    allocation_failure = 1; assert(!driver_work_create(q, increment, &count));
    struct driver_work *work = driver_work_create(q, increment, &count); assert(work);
    assert(driver_work_schedule(work, clock_get_ticks() + 10000) == 1);
    driver_work_put(work); /* Only pending queue reference remains. */
    assert(!driver_workqueue_destroy(&q, 0) && !count); join_workers();
}
static void *creator(void *unused)
{
    (void)unused;
    struct driver_workqueue *q = driver_workqueue_create(); assert(q);
    unsigned count = 0;
    struct driver_work *w = driver_work_create(q, increment, &count); assert(w);
    assert(driver_work_schedule(w, 0) == 1);
    assert(!driver_workqueue_flush(q, clock_get_ticks() + 1000) && count == 1);
    driver_work_put(w); assert(!driver_workqueue_destroy(&q, 0)); return NULL;
}
static void concurrent_startup(void)
{
    pthread_t creators[16];
    for (unsigned i = 0; i < 16; i++) pthread_create(&creators[i], NULL, creator, NULL);
    for (unsigned i = 0; i < 16; i++) pthread_join(creators[i], NULL);
    join_workers();
}
int main(void)
{
    ordered_and_delayed(); self_resubmit(); cancellation(); shutdown_timeout();
    failures_and_pending_reference(); concurrent_startup();
    puts("native workqueue: PASS"); return 0;
}
