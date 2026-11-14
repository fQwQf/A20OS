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
static _Thread_local bool fail_allocation, irq_context;
static struct driver_fence *complete_on_allocation;
void *kmalloc(size_t n)
{
    assert(!host_lock_depth && !irq_context);
    if (fail_allocation) { fail_allocation = false; return NULL; }
    void *p = malloc(n); assert(p);
    __atomic_add_fetch(&allocations, 1, __ATOMIC_RELAXED);
    if (complete_on_allocation) {
        struct driver_fence *fence = complete_on_allocation;
        complete_on_allocation = NULL;
        irq_context = true;
        assert(!driver_fence_signal(fence, -EIO));
        irq_context = false;
    }
    return p;
}
void kfree(void *p)
{
    assert(!host_lock_depth && !irq_context);
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
struct notice {
    struct driver_fence *fence;
    unsigned calls, entered, release;
    int status;
    bool hold;
};
static void notified(void *opaque, int status)
{
    struct notice *n = opaque;
    assert(irq_context && !host_lock_depth && !__atomic_load_n(&linked_count, __ATOMIC_ACQUIRE));
    assert(status && driver_fence_status(n->fence) == status);
    assert(driver_fence_signal(n->fence, -ECANCELED) == -EALREADY);
    n->status = status;
    __atomic_add_fetch(&n->calls, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&n->entered, 1, __ATOMIC_RELEASE);
    /* Test-only pause of callback execution exposes the RUNNING retirement
     * boundary deterministically. Real IRQ notifications must never wait. */
    while (n->hold && !__atomic_load_n(&n->release, __ATOMIC_ACQUIRE)) sched_yield();
}
static void irq_signal(struct driver_fence *fence, int error)
{
    irq_context = true;
    assert(!driver_fence_signal(fence, error));
    irq_context = false;
}
static void *irq_producer(void *opaque)
{
    struct signal_arg *arg = opaque;
    irq_context = true;
    arg->result = driver_fence_signal(arg->fence, arg->error);
    irq_context = false;
    return NULL;
}
static void wait_entered(struct notice *n)
{ while (!__atomic_load_n(&n->entered, __ATOMIC_ACQUIRE)) sched_yield(); }

struct register_race {
    struct driver_fence *fence;
    struct notice notice;
    struct driver_fence_watch *watch;
    pthread_barrier_t start;
    int result;
};
static void *racing_register(void *opaque)
{
    struct register_race *r = opaque;
    int b = pthread_barrier_wait(&r->start);
    assert(b == 0 || b == PTHREAD_BARRIER_SERIAL_THREAD);
    r->result = driver_fence_watch_create(r->fence, notified, &r->notice, &r->watch);
    return NULL;
}
static void *racing_signal(void *opaque)
{
    struct register_race *r = opaque;
    int b = pthread_barrier_wait(&r->start);
    assert(b == 0 || b == PTHREAD_BARRIER_SERIAL_THREAD);
    irq_signal(r->fence, 0);
    return NULL;
}
struct remove_race {
    struct driver_fence_watch **watch;
    int result;
};
static void *racing_remove(void *opaque)
{
    struct remove_race *r = opaque;
    r->result = driver_fence_watch_try_remove(r->watch);
    return NULL;
}

static void watch_case(const char *name)
{
    struct driver_fence *fence = driver_fence_create(); assert(fence);
    struct driver_fence_watch *watch = NULL;
    struct notice n = {.fence = fence};
    if (!strcmp(name, "arguments")) {
        watch = (void *)1;
        assert(driver_fence_watch_create(NULL, notified, &n, &watch) == -EINVAL && !watch);
        watch = (void *)1;
        assert(driver_fence_watch_create(fence, NULL, &n, &watch) == -EINVAL && !watch);
        assert(driver_fence_watch_create(fence, notified, &n, NULL) == -EINVAL);
        assert(driver_fence_watch_try_remove(NULL) == -EINVAL);
        assert(!driver_fence_watch_try_remove(&watch));
    } else if (!strcmp(name, "allocation")) {
        watch = (void *)1; fail_allocation = true;
        assert(driver_fence_watch_create(fence, notified, &n, &watch) == -ENOMEM && !watch);
        assert(!driver_fence_status(fence) && allocations == 1 && !n.calls);
    } else if (!strcmp(name, "terminal")) {
        irq_signal(fence, -EIO);
        watch = (void *)1; fail_allocation = true;
        assert(driver_fence_watch_create(fence, notified, &n, &watch) == -EALREADY && !watch);
        assert(fail_allocation); fail_allocation = false;
        assert(!n.calls && allocations == 1 && driver_fence_status(fence) == -EIO);
    } else if (!strcmp(name, "allocation-signal")) {
        complete_on_allocation = fence; watch = (void *)1;
        assert(driver_fence_watch_create(fence, notified, &n, &watch) == -EALREADY && !watch);
        assert(!complete_on_allocation && !n.calls && allocations == 1);
        assert(driver_fence_status(fence) == -EIO);
    } else if (!strcmp(name, "pending-owner")) {
        assert(!driver_fence_watch_create(fence, notified, &n, &watch));
        driver_fence_put(fence); fence = NULL;
        assert(allocations == 2 && !n.calls);
        assert(!driver_fence_watch_try_remove(&watch) && !watch && !allocations);
    } else if (!strcmp(name, "complete-owner")) {
        assert(!driver_fence_watch_create(fence, notified, &n, &watch));
        irq_signal(fence, 0);
        driver_fence_put(fence); fence = NULL;
        assert(allocations == 2 && n.calls == 1 && n.status == 1);
        assert(!driver_fence_watch_try_remove(&watch) && !watch && !allocations);
    } else if (!strcmp(name, "notify") || !strcmp(name, "error")) {
        assert(!driver_fence_watch_create(fence, notified, &n, &watch));
        assert(!n.calls && allocations == 2 && !driver_fence_status(fence));
        assert(driver_fence_signal(fence, 1) == -EINVAL && !n.calls);
        int error = !strcmp(name, "error") ? -EIO : 0;
        irq_signal(fence, error);
        assert(n.calls == 1 && n.status == (error ? error : 1) && allocations == 2);
        assert(driver_fence_signal(fence, 0) == -EALREADY && n.calls == 1);
        assert(!driver_fence_watch_try_remove(&watch) && !watch);
        assert(!driver_fence_watch_try_remove(&watch));
    } else if (!strcmp(name, "cancel")) {
        assert(!driver_fence_watch_create(fence, notified, &n, &watch));
        assert(!driver_fence_watch_try_remove(&watch) && !watch);
        assert(!driver_fence_status(fence) && allocations == 1);
        irq_signal(fence, 0); assert(!n.calls);
    } else if (!strcmp(name, "many")) {
        enum { COUNT = 128 };
        struct driver_fence_watch *watches[COUNT]; struct notice notices[COUNT];
        for (unsigned i=0;i<COUNT;i++) {
            notices[i] = (struct notice){.fence=fence};
            assert(!driver_fence_watch_create(fence, notified, &notices[i], &watches[i]));
        }
        /* Head, tail and middle removals exercise both neighbouring links. */
        for (unsigned i=0;i<COUNT;i+=3) assert(!driver_fence_watch_try_remove(&watches[i]));
        assert(!driver_fence_watch_try_remove(&watches[COUNT-1]));
        irq_signal(fence, -ECANCELED);
        for (unsigned i=0;i<COUNT;i++) {
            assert(notices[i].calls == (i%3 && i != COUNT-1 ? 1u : 0u));
            assert(!driver_fence_watch_try_remove(&watches[i]) && !watches[i]);
        }
    } else if (!strcmp(name, "running") || !strcmp(name, "cancel-terminal-pending") ||
               !strcmp(name, "waiter-progress")) {
        n.hold = true;
        assert(!driver_fence_watch_create(fence, notified, &n, &watch));
        struct notice later = {.fence=fence};struct driver_fence_watch *second=NULL;
        if (!strcmp(name,"cancel-terminal-pending")) {
            later.hold = true;
            assert(!driver_fence_watch_create(fence, notified, &later, &second));
        }
        pthread_t waiting;
        if (!strcmp(name,"waiter-progress")) {
            driver_fence_get(fence);assert(!pthread_create(&waiting,NULL,waiter,fence));
            while (!__atomic_load_n(&linked_count,__ATOMIC_ACQUIRE)) sched_yield();
        }
        struct signal_arg arg={fence,0,0};pthread_t producer;
        assert(!pthread_create(&producer,NULL,irq_producer,&arg));
        if (second) {
            /* Either registration may be selected first: order is not part
             * of the interface. Pause that one and cancel the other. */
            while (!__atomic_load_n(&n.entered,__ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&later.entered,__ATOMIC_ACQUIRE)) sched_yield();
        } else wait_entered(&n);
        struct notice *active=&n,*unselected=&later;
        struct driver_fence_watch **running=&watch,**other=&second;
        if (__atomic_load_n(&later.entered,__ATOMIC_ACQUIRE)) {
            active=&later;unselected=&n;running=&second;other=&watch;
        }
        struct driver_fence_watch *saved=*running;
        assert(driver_fence_status(fence)==1);
        assert(driver_fence_watch_try_remove(running)==-EBUSY && *running==saved);
        if (*other) assert(!driver_fence_watch_try_remove(other) && !*other && !unselected->calls);
        if (!strcmp(name,"waiter-progress")) assert(!pthread_join(waiting,NULL));
        __atomic_store_n(&active->release,1,__ATOMIC_RELEASE);
        assert(!pthread_join(producer,NULL) && !arg.result && active->calls==1);
        assert(!driver_fence_watch_try_remove(running) && !watch && !second);
        assert(!unselected->calls);
    } else if (!strcmp(name, "register-race")) {
        for (unsigned i=0;i<200;i++) {
            struct register_race r={.fence=fence,.notice={.fence=fence}};
            assert(!pthread_barrier_init(&r.start,NULL,2));pthread_t a,b;
            assert(!pthread_create(&a,NULL,racing_register,&r));
            assert(!pthread_create(&b,NULL,racing_signal,&r));
            assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
            assert(r.result==0 || r.result==-EALREADY);
            assert(r.notice.calls==(r.result==0 ? 1u : 0u));
            assert(!driver_fence_watch_try_remove(&r.watch) && !r.watch);
            assert(!pthread_barrier_destroy(&r.start));
            driver_fence_put(fence);fence=driver_fence_create();assert(fence);
        }
    } else if (!strcmp(name, "remove-race")) {
        for (unsigned i=0;i<200;i++) {
            n=(struct notice){.fence=fence};
            assert(!driver_fence_watch_create(fence,notified,&n,&watch));
            struct signal_arg arg={fence,0,0};struct remove_race r={&watch,0};pthread_t a,b;
            assert(!pthread_create(&a,NULL,irq_producer,&arg));
            assert(!pthread_create(&b,NULL,racing_remove,&r));
            assert(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !arg.result);
            assert(r.result==0 || r.result==-EBUSY);
            assert(n.calls<=1 && (r.result!=-EBUSY || n.calls==1));
            assert(!driver_fence_watch_try_remove(&watch) && !watch);
            driver_fence_put(fence);fence=driver_fence_create();assert(fence);
        }
    } else if (!strcmp(name, "producers")) {
        struct notice peer={.fence=fence};struct driver_fence_watch *second;
        assert(!driver_fence_watch_create(fence,notified,&n,&watch));
        assert(!driver_fence_watch_create(fence,notified,&peer,&second));
        struct signal_arg a={fence,0,0},b={fence,-EIO,0};pthread_t x,y;
        assert(!pthread_create(&x,NULL,irq_producer,&a));assert(!pthread_create(&y,NULL,irq_producer,&b));
        assert(!pthread_join(x,NULL) && !pthread_join(y,NULL));
        assert((a.result==0 && b.result==-EALREADY) || (b.result==0 && a.result==-EALREADY));
        assert(n.calls==1 && peer.calls==1 && n.status==peer.status && n.status==(a.result==0 ? 1:-EIO));
        assert(!driver_fence_watch_try_remove(&watch) && !driver_fence_watch_try_remove(&second));
    } else assert(!"unknown watch case");
    driver_fence_put(fence);
    assert(!allocations && !linked_count);
    puts("native fence notification: PASS");
}

int main(int argc, char **argv)
{
    if (argc == 2) { watch_case(argv[1]); return 0; }
    assert(argc == 1);
    boundary_cases(); multiple_waiters(); competing_producers();
    puts("native asynchronous fences: PASS"); return 0;
}
