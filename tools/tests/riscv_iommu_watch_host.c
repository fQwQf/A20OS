#define main queue_existing_main
#define kcalloc queue_test_kcalloc
#include "driver_workqueue_host.c"
#undef kcalloc
#undef main
#include "drivers/core/driver_core.h"
#include "drivers/core/riscv_iommu_internal.h"

static int fail_after = -1;
void *kcalloc(size_t n, size_t size)
{
    if (fail_after == 0) { fail_after = -1; return NULL; }
    if (fail_after > 0) fail_after--;
    return queue_test_kcalloc(n, size);
}
device_t *device_get(device_t *d)
{ assert(__atomic_load_n(&d->references, __ATOMIC_RELAXED)); __atomic_add_fetch(&d->references, 1, __ATOMIC_RELAXED); return d; }
void device_put(device_t *d)
{ assert(__atomic_fetch_sub(&d->references, 1, __ATOMIC_RELAXED) > 1); }

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static int owner = -1;
static unsigned polls[4];
static struct riscv_iommu_dma_fault current;
struct notifications {
    unsigned calls;
    int status, self_result;
    bool block, self_stop;
    struct gate gate;
    struct riscv_iommu_dma_watch **watch;
    struct riscv_iommu_dma_fault last;
};
int riscv_iommu_native_identity(device_t *d, uint16_t *devid, int *token)
{
    (void)d; pthread_mutex_lock(&state_lock);
    *devid = 8; *token = owner;
    int ret = *token < 0 ? 0 : -ENODEV;
    pthread_mutex_unlock(&state_lock); return ret;
}
int riscv_iommu_fault_poll_owned(device_t *d, int token, struct riscv_iommu_dma_fault *out)
{
    (void)d; assert(!host_lock_depth && token < 0 && token >= -3);
    pthread_mutex_lock(&state_lock);
    polls[-token]++;
    memset(out, 0, sizeof(*out));
    int ret = token == owner ? 0 : -ENODEV;
    if (!ret) *out = current;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_lock); return ret;
}
static void notify(void *opaque, int status, const struct riscv_iommu_dma_fault *fault)
{
    struct notifications *n = opaque;
    assert(!host_lock_depth);
    if (n->self_stop) n->self_result = riscv_iommu_dma_watch_stop(n->watch, 0);
    pthread_mutex_lock(&state_lock);
    n->calls++; n->status = status; n->last = *fault;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_lock);
    if (n->block) blocking(&n->gate);
}
static void await(unsigned token, unsigned minimum_polls, struct notifications *n, unsigned calls)
{
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 2;
    pthread_mutex_lock(&state_lock);
    while (polls[token] < minimum_polls || n->calls < calls)
        assert(!pthread_cond_timedwait(&state_changed, &state_lock, &until));
    pthread_mutex_unlock(&state_lock);
}
int main(void)
{
    device_t d = {.references=1};
    struct riscv_iommu_dma_watch *watch = NULL, *replacement = NULL;
    struct notifications first = {0}, second = {0};
    no_task = 1;
    assert(riscv_iommu_dma_watch_start(&d, 5, notify, &first, &watch) == -EAGAIN && !watch);
    no_task = 0;
    for (int failure = 0; failure < 3; failure++) {
        fail_after = failure;
        assert(riscv_iommu_dma_watch_start(&d, 5, notify, &first, &watch) == -ENOMEM && !watch);
        join_workers(); assert(d.references == 1);
    }
    assert(!riscv_iommu_dma_watch_start(&d, 5, notify, &first, &watch));
    await(1, 3, &first, 0);
    pthread_mutex_lock(&state_lock);
    assert(!first.calls);
    current = (struct riscv_iommu_dma_fault){.count=1, .cause=15, .blocked=true, .isolation_error=-EIO};
    pthread_mutex_unlock(&state_lock);
    await(1, 0, &first, 1);
    pthread_mutex_lock(&state_lock); unsigned before = polls[1]; pthread_mutex_unlock(&state_lock);
    await(1, before + 3, &first, 1);
    pthread_mutex_lock(&state_lock);
    assert(first.calls == 1);
    current.isolation_error = 0; current.isolated = true;
    pthread_mutex_unlock(&state_lock);
    await(1, 0, &first, 2);
    pthread_mutex_lock(&state_lock);
    assert(first.last.isolated);
    owner = -2; memset(&current, 0, sizeof(current));
    pthread_mutex_unlock(&state_lock);
    await(1, 0, &first, 3);
    pthread_mutex_lock(&state_lock); before = polls[1]; assert(first.status == -ENODEV); pthread_mutex_unlock(&state_lock);
    assert(!riscv_iommu_dma_watch_start(&d, 5, notify, &second, &replacement));
    await(2, 4, &second, 0);
    pthread_mutex_lock(&state_lock); assert(polls[1] == before && first.calls == 3 && !second.calls); pthread_mutex_unlock(&state_lock);
    assert(!riscv_iommu_dma_watch_stop(&watch, 0) && !watch);
    assert(!riscv_iommu_dma_watch_stop(&replacement, 0)); join_workers();

    struct notifications blocked = {.block=true, .self_stop=true, .gate=GATE_INIT, .watch=&watch};
    pthread_mutex_lock(&state_lock); owner = -3; current.count = 1; pthread_mutex_unlock(&state_lock);
    assert(!riscv_iommu_dma_watch_start(&d, 5, notify, &blocked, &watch));
    wait_entered(&blocked.gate);
    assert(blocked.self_result == -EDEADLK && watch);
    assert(riscv_iommu_dma_watch_stop(&watch, clock_get_ticks() + 5) == -ETIMEDOUT);
    assert(watch && d.references == 2);
    release_gate(&blocked.gate);
    assert(!riscv_iommu_dma_watch_stop(&watch, clock_get_ticks() + 2000) && !watch);
    join_workers(); assert(d.references == 1 && blocked.calls == 1);
    assert(!riscv_iommu_dma_watch_stop(&watch, 0));
    puts("native IOMMU watch: owner generations, notifications, timeout/retry PASS");
    return 0;
}
