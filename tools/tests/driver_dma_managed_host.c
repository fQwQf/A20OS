/* Reuse the allocator and provider doubles; execute real domain code. */
#define main dma_existing_main
#include "driver_dma_mapping_host.c"
#undef main
#include <pthread.h>
#include <sched.h>

_Thread_local int host_closer;
int host_closing_wait;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int entered, proceed, shutdown_result = 99;
static struct driver_dma_domain *test_domain;
static struct driver_dma_mapping *test_mapping;
static pfn_t test_pages[] = {100};

static int blocked_map(void *context, uint64_t physical,
                         enum driver_dma_direction direction, uint64_t *address,
                         bool *needs_unmap)
{
    assert(!pthread_mutex_lock(&gate));
    entered = 1;
    assert(!pthread_cond_broadcast(&changed));
    while (!proceed) assert(!pthread_cond_wait(&changed, &gate));
    assert(!pthread_mutex_unlock(&gate));
    return managed_map(context, physical, direction, address, needs_unmap);
}
static void *mapper(void *unused)
{
    (void)unused;
    assert(!driver_dma_map_pages(test_domain, test_pages, 1,
                                 DRIVER_DMA_BIDIRECTIONAL, &test_mapping));
    return NULL;
}
static void *closer(void *unused)
{
    (void)unused;
    host_closer = 1;
    int ret = driver_dma_domain_shutdown(test_domain);
    __atomic_store_n(&shutdown_result, ret, __ATOMIC_RELEASE);
    return NULL;
}
int main(void)
{
    setup();
    device_t d = {.references=1};
    struct provider p = {.offset=0x200000};
    struct driver_dma_managed_ops operations = managed_ops;
    operations.map_page = blocked_map;
    assert(!driver_dma_domain_create_managed(&d, &operations, &p, &test_domain));
    pthread_t map_thread, close_thread;
    assert(!pthread_create(&map_thread, NULL, mapper, NULL));
    assert(!pthread_mutex_lock(&gate));
    while (!entered) assert(!pthread_cond_wait(&changed, &gate));
    assert(!pthread_create(&close_thread, NULL, closer, NULL));
    while (!__atomic_load_n(&host_closing_wait, __ATOMIC_ACQUIRE)) sched_yield();
    /* Shutdown tried to enter while the callback still owns the domain. */
    assert(__atomic_load_n(&shutdown_result, __ATOMIC_ACQUIRE) == 99);
    proceed = 1;
    assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(map_thread, NULL) && !pthread_join(close_thread, NULL));
    assert(shutdown_result == -EBUSY && !p.detaches && test_mapping);
    assert(!driver_dma_try_unmap_pages(&test_mapping));
    assert(!driver_dma_domain_shutdown(test_domain) && p.detaches == 1);
    driver_dma_domain_put(test_domain);
    assert(!allocations && p.releases == 1 && d.references == 1);
    puts("managed DMA domain: map/shutdown serialization PASS");
    return 0;
}
