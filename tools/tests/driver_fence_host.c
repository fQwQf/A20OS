#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/core/fence.h"
#include "core/sync.h"
#include "core/errno.h"

_Thread_local unsigned host_lock_depth;
static unsigned allocations, linked_count;
void *kmalloc(size_t n)
{
    assert(!host_lock_depth);
    void *p = malloc(n); assert(p);
    __atomic_add_fetch(&allocations, 1, __ATOMIC_RELAXED); return p;
}
void kfree(void *p)
{
    assert(!host_lock_depth);
    if (p) { __atomic_sub_fetch(&allocations, 1, __ATOMIC_RELAXED); free(p); }
}
uint64_t clock_get_ticks(void) { return 10; }

enum { NORMAL, EARLY_SIGNAL, COMMIT_SIGNAL, TIMEOUT, TIMEOUT_SIGNAL,
       INTERRUPTED, NO_TOKEN, LINK_FAILURE, SPURIOUS };
static _Thread_local int scenario, prepares;
static _Thread_local struct driver_fence *current_fence;
struct host_task {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    int ready;
    proc_wake_reason_t reason;
    uint64_t seq;
};
static _Thread_local struct host_task task;
static _Thread_local int initialized;

void wait_queue_init(wait_queue_t *q)
{ spin_init(&q->lock); q->head = NULL; q->generation = 0; }
bool wait_queue_link(wait_queue_t *q, wait_queue_entry_t *entry,
                      proc_wait_token_t token, uintptr_t key)
{
    if (scenario == LINK_FAILURE) return false;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    entry->task = token.task; entry->key = key;
    entry->next = q->head; entry->queue = q; entry->linked = true;
    q->head = entry;
    __atomic_add_fetch(&linked_count, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&q->lock, flags);
    return true;
}
void wait_queue_unlink(wait_queue_t *q, wait_queue_entry_t *entry)
{
    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t **p = &q->head;
    while (*p && *p != entry) p = &(*p)->next;
    if (*p) {
        *p = entry->next; entry->linked = false;
        __atomic_sub_fetch(&linked_count, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&q->lock, flags);
}
unsigned wait_queue_wake_all(wait_queue_t *q, uintptr_t key, proc_wake_reason_t reason)
{
    assert(!host_lock_depth);
    unsigned count = 0;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    while (q->head) {
        wait_queue_entry_t *e = q->head;
        assert(e->key == key); q->head = e->next; e->linked = false;
        struct host_task *t = (struct host_task *)e->task;
        pthread_mutex_lock(&t->lock);
        t->reason = reason; t->ready = 1; pthread_cond_signal(&t->condition);
        pthread_mutex_unlock(&t->lock);
        __atomic_sub_fetch(&linked_count, 1, __ATOMIC_RELEASE); count++;
    }
    spin_unlock_irqrestore(&q->lock, flags);
    return count;
}
proc_wait_token_t proc_park_prepare(proc_wait_mode_t mode, uint64_t deadline)
{
    (void)deadline;
    assert(!host_lock_depth);
    if (scenario == INTERRUPTED) assert(mode == PROC_WAIT_INTERRUPTIBLE);
    prepares++;
    if (scenario == NO_TOKEN) return (proc_wait_token_t){0};
    if (!initialized) {
        pthread_mutex_init(&task.lock, NULL);
        pthread_cond_init(&task.condition, NULL); initialized = 1;
    }
    pthread_mutex_lock(&task.lock);
    task.ready = 0; task.seq++;
    pthread_mutex_unlock(&task.lock);
    if (scenario == EARLY_SIGNAL) assert(!driver_fence_signal(current_fence, 0));
    return (proc_wait_token_t){.task=(struct task_t *)&task, .seq=task.seq};
}
int proc_park_cancel(proc_wait_token_t token)
{ (void)token; assert(!host_lock_depth); return 0; }
void proc_park_finish(proc_wait_token_t token)
{ (void)token; assert(!host_lock_depth); }
proc_wake_reason_t proc_park_commit(proc_wait_token_t token)
{
    assert(!host_lock_depth);
    if (scenario == TIMEOUT) return PROC_WAKE_TIMEOUT;
    if (scenario == TIMEOUT_SIGNAL) {
        assert(!driver_fence_signal(current_fence, -EIO)); return PROC_WAKE_TIMEOUT;
    }
    if (scenario == INTERRUPTED) return PROC_WAKE_SIGNAL;
    if (scenario == SPURIOUS && prepares == 1) return PROC_WAKE_EVENT;
    if (scenario == COMMIT_SIGNAL || scenario == SPURIOUS)
        assert(!driver_fence_signal(current_fence, 0));
    struct host_task *t = (struct host_task *)token.task;
    pthread_mutex_lock(&t->lock);
    while (!t->ready) pthread_cond_wait(&t->condition, &t->lock);
    proc_wake_reason_t reason = t->reason;
    pthread_mutex_unlock(&t->lock);
    return reason;
}

static void boundary_cases(void)
{
    const int expected[] = {0, 0, 0, -ETIMEDOUT, -EIO, -EINTR, -EAGAIN, -EAGAIN, 0};
    for (int test = EARLY_SIGNAL; test <= SPURIOUS; test++) {
        scenario = test; prepares = 0;
        current_fence = driver_fence_create(); assert(current_fence);
        assert(driver_fence_signal(current_fence, 1) == -EINVAL);
        assert(driver_fence_wait(current_fence, 20, test == INTERRUPTED) == expected[test]);
        assert(!linked_count);
        if (test == TIMEOUT || test == INTERRUPTED || test == NO_TOKEN || test == LINK_FAILURE) {
            assert(driver_fence_status(current_fence) == 0); /* Wait never cancels DMA. */
            assert(!driver_fence_signal(current_fence, -ECANCELED));
        }
        assert(driver_fence_signal(current_fence, 0) == -EALREADY);
        driver_fence_put(current_fence); assert(!allocations);
    }
    scenario = NORMAL;
    current_fence = driver_fence_create();
    assert(driver_fence_wait(current_fence, 10, false) == -ETIMEDOUT);
    assert(!driver_fence_status(current_fence));
    assert(!driver_fence_signal(current_fence, 0));
    assert(!driver_fence_wait(current_fence, 1, false)); /* Completed before expired deadline. */
    driver_fence_put(current_fence);
}
static void *waiter(void *p)
{
    struct driver_fence *fence = p;
    assert(!driver_fence_wait(fence, 0, false));
    driver_fence_put(fence); return NULL;
}
static void multiple_waiters(void)
{
    enum { COUNT = 80 };
    pthread_t threads[COUNT];
    struct driver_fence *fence = driver_fence_create();
    for (unsigned i = 0; i < COUNT; i++) {
        driver_fence_get(fence);
        assert(!pthread_create(&threads[i], NULL, waiter, fence));
    }
    while (__atomic_load_n(&linked_count, __ATOMIC_ACQUIRE) != COUNT) sched_yield();
    assert(!driver_fence_signal(fence, 0));
    driver_fence_put(fence); /* Producers do not own the waiters' references. */
    for (unsigned i = 0; i < COUNT; i++) assert(!pthread_join(threads[i], NULL));
    assert(!linked_count && !allocations);
}
struct signal_arg { struct driver_fence *fence; int error, result; };
static void *signaler(void *p)
{
    struct signal_arg *arg = p;
    arg->result = driver_fence_signal(arg->fence, arg->error); return NULL;
}
static void competing_producers(void)
{
    for (unsigned i = 0; i < 100; i++) {
        struct driver_fence *fence = driver_fence_create();
        struct signal_arg a = {fence, 0, 0}, b = {fence, -EIO, 0};
        pthread_t first, second;
        assert(!pthread_create(&first, NULL, signaler, &a));
        assert(!pthread_create(&second, NULL, signaler, &b));
        pthread_join(first, NULL); pthread_join(second, NULL);
        assert((a.result == 0 && b.result == -EALREADY) ||
               (b.result == 0 && a.result == -EALREADY));
        assert(driver_fence_status(fence) == (a.result == 0 ? 1 : -EIO));
        driver_fence_put(fence);
    }
    assert(!allocations);
}
int main(void)
{
    boundary_cases(); multiple_waiters(); competing_producers();
    puts("native asynchronous fences: PASS"); return 0;
}
