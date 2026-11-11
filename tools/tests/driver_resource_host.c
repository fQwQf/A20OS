/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/core/driver_core.h"
#include "core/errno.h"

unsigned host_cpu;
_Thread_local unsigned host_lock_depth;
int host_trylock_blocked;
static int allocations, fail_after = -1, maps, unmaps, fail_map;
static int stopped, removed, released, block_unmap;
static unsigned char registers[4096];
static resource_t region = {.type = RES_MMIO, .start = 0x1000, .end = 0x1fff};
static device_t a = {
    .name = "claim-a", .res = &region, .res_count = 1, .references = 1};
static device_t b = {
    .name = "claim-b", .res = &region, .res_count = 1, .references = 1};
static pthread_barrier_t unmap_enter, unmap_continue;

void *kmalloc(size_t size) {
    if (!fail_after)
        return NULL;
    if (fail_after > 0)
        fail_after--;
    void *p = malloc(size);
    if (p)
        __atomic_add_fetch(&allocations, 1, __ATOMIC_RELAXED);
    return p;
}
void *kcalloc(size_t n, size_t size) {
    void *p = kmalloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}
void *krealloc(void *p, size_t size) {
    if (!p)
        return kmalloc(size);
    return realloc(p, size);
}
void kfree(void *p) {
    if (p) {
        __atomic_sub_fetch(&allocations, 1, __ATOMIC_RELAXED);
        free(p);
    }
}
static _Thread_local int task_marker;
struct task_t *proc_current(void) {
    return (struct task_t *)&task_marker;
}
void host_log(const char *fmt, ...) {
    (void)fmt;
}
void panic(const char *fmt, ...) {
    (void)fmt;
    abort();
}
int firmware_device_prepare(device_t *dev) {
    (void)dev;
    return 0;
}
void netlink_uevent_emit(const char *a, const char *b, const char *c,
                         uint64_t d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
}
__asm__(".global __driver_init_start\n.global __driver_init_end\n"
        "__driver_init_start:\n__driver_init_end:\n");

void *arch_device_map_mmio(uint64_t physical, size_t size, uint32_t flags) {
    (void)flags;
    assert(!host_lock_depth && physical == 0x1000 && size == 4096);
    maps++;
    return fail_map ? NULL : registers;
}
void arch_device_unmap_mmio(void *address, size_t size) {
    assert(!host_lock_depth && address == registers && size == 4096);
    driver_resource_claim_t *other = NULL;
    /* Allocation failure triggered this rollback; let the independent
     * contender allocate so its result tests ownership rather than memory. */
    int saved_failure = fail_after;
    fail_after = -1;
    assert(driver_resource_request(&b, &region, &other) == -EBUSY && !other);
    fail_after = saved_failure;
    if (block_unmap) {
        pthread_barrier_wait(&unmap_enter);
        pthread_barrier_wait(&unmap_continue);
    }
    unmaps++;
}
static void owner_release(device_t *dev) {
    (void)dev;
    assert(!host_lock_depth);
    released++;
}

static void intervals(void) {
    driver_resource_claim_t *first, *second, *third;
    resource_t copy = region;
    assert(!driver_resource_request(&a, &copy, &first) && a.references == 2);
    copy.start = 0x2000;
    copy.end = 0x2fff; /* Stored bounds do not borrow this struct. */
    assert(driver_resource_request(&b, &region, &second) == -EBUSY && !second &&
           b.references == 1);
    assert(!driver_resource_request(&a, &region, &second) && a.references == 3);
    driver_resource_release(first);
    assert(driver_resource_request(&b, &region, &first) == -EBUSY && !first);
    driver_resource_release(second);
    assert(a.references == 1 && !driver_resource_request(&b, &region, &first));
    driver_resource_release(first);
    copy = (resource_t){
        .type = RES_MMIO, .start = ~(uint64_t)0 - 3, .end = ~(uint64_t)0};
    assert(!driver_resource_request(&a, &copy, &first));
    assert(driver_resource_request(&b, &copy, &second) == -EBUSY);
    copy.start -= 4;
    copy.end -= 4;
    assert(!driver_resource_request(&b, &copy,
                                    &second)); /* Adjacent inclusive ranges. */
    driver_resource_release(second);
    driver_resource_release(first);
    copy = (resource_t){.type = RES_IOPORT, .start = 0x1000, .end = 0x1fff};
    assert(!driver_resource_request(&a, &copy, &first));
    assert(!driver_resource_request(&b, &region,
                                    &second)); /* Separate namespaces. */
    copy.start = 0x1800;
    copy.end = 0x1800;
    assert(driver_resource_request(&b, &copy, &third) == -EBUSY && !third);
    driver_resource_release(first);
    driver_resource_release(second);
    device_t retired = {
        .name = "retired", .references = 1, .release = owner_release};
    assert(!driver_resource_request(&retired, &region, &first));
    device_put(&retired);
    assert(!released && retired.references == 1);
    driver_resource_release(first);
    assert(released == 1 && !retired.references);
    assert(driver_resource_request(&retired, &region, &first) == -ENODEV &&
           !first);
    driver_resource_release(NULL);
}
static void invalid(void) {
    driver_resource_claim_t *claim = (void *)1;
    resource_t bad = region;
    bad.end = bad.start - 1;
    assert(driver_resource_request(&a, &bad, &claim) == -EINVAL && !claim);
    bad = region;
    bad.type = RES_IRQ;
    assert(driver_resource_request(&a, &bad, &claim) == -EINVAL && !claim);
    bad = (resource_t){.type = RES_IOPORT, .start = 0xffff, .end = 0x10000};
    assert(driver_resource_request(&a, &bad, &claim) == -EINVAL && !claim);
    assert(driver_resource_request(NULL, &region, &claim) == -EINVAL && !claim);
    assert(driver_resource_request(&a, NULL, &claim) == -EINVAL && !claim);
    assert(driver_resource_request(&a, &region, NULL) == -EINVAL);
    resource_t foreign = region;
    void *address = (void *)1;
    assert(device_map_resource_exclusive(&a, &foreign, &address) == -EINVAL &&
           !address);
    assert(device_claim_resource(&a, &foreign) == -EINVAL);
    assert(device_map_resource_exclusive(&a, &region, NULL) == -EINVAL);
    resource_t saved = region;
    region.end = ~(uint64_t)0;
    region.start = 0;
    assert(device_map_resource_exclusive(&a, &region, &address) == -EOVERFLOW &&
           !address);
    region = saved;
}
static void failures_and_managed(void) {
    int before = allocations;
    for (int allocation = 0; allocation < 3; allocation++) {
        fail_after = allocation;
        void *address = (void *)1;
        assert(device_map_resource_exclusive(&a, &region, &address) ==
                   -ENOMEM &&
               !address);
        fail_after = -1;
        assert(!a.resource_actions && a.references == 1 &&
               allocations == before);
        driver_resource_claim_t *claim;
        assert(!driver_resource_request(&b, &region, &claim));
        driver_resource_release(claim);
    }
    fail_map = 1;
    assert(!device_map_resource(&a, &region));
    fail_map = 0;
    assert(allocations == before && a.references == 1);
    fail_after = 1;
    assert(device_claim_resource(&a, &region) == -ENOMEM);
    fail_after = -1;
    assert(allocations == before && a.references == 1 && !a.resource_actions);
    assert(!device_claim_resource(&a, &region));
    assert(device_map_mmio(&a, 0) ==
           registers); /* Same owner may acquire/map again. */
    assert(a.references == 3);
    int before_maps = maps;
    void *conflicting = (void *)1;
    assert(device_map_resource_exclusive(&b, &region, &conflicting) == -EBUSY &&
           !conflicting);
    assert(!device_map_resource(&b, &region) && maps == before_maps);
    assert(b.references == 1 && !b.resource_actions);
    driver_resource_claim_t *claim;
    assert(driver_resource_request(&b, &region, &claim) == -EBUSY);
    device_release_resources(&a);
    assert(allocations == before && a.references == 1 && !a.resource_actions);
    assert(!driver_resource_request(&b, &region, &claim));
    driver_resource_release(claim);
}
static void *release_maps(void *unused) {
    (void)unused;
    device_release_resources(&a);
    return NULL;
}
static void draining_unmap(void) {
    assert(device_map_resource(&a, &region) == registers);
    assert(!pthread_barrier_init(&unmap_enter, NULL, 2));
    assert(!pthread_barrier_init(&unmap_continue, NULL, 2));
    block_unmap = 1;
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, release_maps, NULL));
    pthread_barrier_wait(&unmap_enter);
    driver_resource_claim_t *claim;
    assert(driver_resource_request(&b, &region, &claim) == -EBUSY && !claim);
    pthread_barrier_wait(&unmap_continue);
    assert(!pthread_join(worker, NULL));
    block_unmap = 0;
    assert(!driver_resource_request(&b, &region, &claim));
    driver_resource_release(claim);
    pthread_barrier_destroy(&unmap_enter);
    pthread_barrier_destroy(&unmap_continue);
}
static pthread_barrier_t start, attempted, proceed;
struct contender {
    device_t *owner;
    int result;
    driver_resource_claim_t *claim;
};
static void *compete(void *arg) {
    struct contender *c = arg;
    pthread_barrier_wait(&start);
    c->result = driver_resource_request(c->owner, &region, &c->claim);
    pthread_barrier_wait(&attempted);
    pthread_barrier_wait(&proceed);
    driver_resource_release(c->claim);
    return NULL;
}
static void concurrency(void) {
    pthread_barrier_init(&start, NULL, 3);
    pthread_barrier_init(&attempted, NULL, 3);
    pthread_barrier_init(&proceed, NULL, 3);
    for (unsigned i = 0; i < 16; i++) {
        struct contender c[2] = {{.owner = &a}, {.owner = &b}};
        pthread_t workers[2];
        for (unsigned k = 0; k < 2; k++)
            assert(!pthread_create(&workers[k], NULL, compete, &c[k]));
        pthread_barrier_wait(&start);
        pthread_barrier_wait(&attempted);
        assert((!c[0].result && c[1].result == -EBUSY) ||
               (!c[1].result && c[0].result == -EBUSY));
        pthread_barrier_wait(&proceed);
        for (unsigned k = 0; k < 2; k++)
            assert(!pthread_join(workers[k], NULL));
        assert(a.references == 1 && b.references == 1);
    }
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&attempted);
    pthread_barrier_destroy(&proceed);
}
static int match(device_t *d, const driver_t *drv) {
    (void)d;
    (void)drv;
    return 1;
}
static int fail_probe;
static int probe(device_t *dev) {
    assert(device_map_resource(dev, &region) == registers);
    return fail_probe ? -EIO : 0;
}
static void quiesce(device_t *dev) {
    (void)dev;
    stopped++;
    driver_resource_claim_t *claim;
    assert(driver_resource_request(&b, &region, &claim) == -EBUSY);
}
static int remove_device(device_t *dev) {
    (void)dev;
    assert(stopped == removed + 1);
    removed++;
    driver_resource_claim_t *claim;
    assert(driver_resource_request(&b, &region, &claim) == -EBUSY);
    return 0;
}
static void lifecycle(void) {
    bus_type_t bus = {.name = "resource", .match = match};
    driver_t driver = {.name = "resource",
                       .bus = &bus,
                       .probe = probe,
                       .quiesce = quiesce,
                       .remove = remove_device};
    device_t dev = {
        .name = "resource", .bus = &bus, .res = &region, .res_count = 1};
    assert(!bus_register(&bus) && !driver_register(&driver));
    assert(!device_register(&dev) && dev.drv == &driver);
    device_unregister(&dev);
    assert(stopped == 1 && removed == 1 && dev.references == 1);
    fail_probe = 1;
    assert(!device_register(&dev) && !dev.drv && dev.probe_error == -EIO);
    assert(!dev.resource_actions && dev.references == 2);
    driver_resource_claim_t *claim;
    assert(!driver_resource_request(&b, &region, &claim));
    driver_resource_release(claim);
    device_unregister(&dev);
    assert(dev.references == 1);
    assert(!driver_unregister(&driver));
    bus_unregister(&bus);
    device_put(&dev);
}
static void inventory(void) {
    driver_resource_claim_t *claim = (void *)1;
    uint64_t end = 99;
    assert(driver_resource_request_checked(&a, &region, &claim) == -ENODEV &&
           !claim);
    assert(!device_register(&a) && !device_register(&b));
    assert(!a.drv && !b.drv);
    assert(driver_resource_check_range(&a, &region, &end) == -EBUSY &&
           end == region.end);
    assert(!driver_resource_check_claims(
        &a, &region)); /* An unbound declaration is not a basic claim. */
    assert(driver_resource_request_checked(&a, &region, &claim) == -EBUSY &&
           !claim);
    resource_t physical_ram = region;
    physical_ram.type = RES_MEM;
    b.res = &physical_ram;
    assert(driver_resource_request_checked(&a, &region, &claim) == -EBUSY &&
           !claim);
    physical_ram.end = physical_ram.start - 1;
    assert(driver_resource_check_range(&a, &region, &end) == -EINVAL && !end);
    b.res = &region;
    device_unregister(&b);
    fail_after = 0;
    assert(driver_resource_request_checked(&a, &region, &claim) == -ENOMEM &&
           !claim);
    fail_after = -1;
    assert(a.references == 2);
    assert(!driver_resource_check_range(&a, &region, &end) && !end);
    assert(driver_resource_check_range(NULL, &region, &end) == -EBUSY &&
           end == region.end);
    assert(!driver_resource_request_checked(&a, &region, &claim));
    device_t parent = {.name = "inventory-parent", .references = 1};
    b.parent = &parent;
    assert(device_register(&b) == -EBUSY && parent.references == 1 &&
           !b.parent_reference);
    b.parent = NULL;
    device_unregister(&a); /* The retained lease still rejects publication. */
    assert(a.references == 2 && device_register(&b) == -EBUSY);
    driver_resource_claim_t *second;
    assert(driver_resource_request_checked(&a, &region, &second) == -ENODEV &&
           !second);
    assert(driver_resource_check_claims(&b, &region) == -EBUSY);
    driver_resource_release(claim);
    assert(a.references == 1 && !device_register(&b));
    device_unregister(&b);
    assert(a.references == 1 && b.references == 1);
}
struct inventory_race {
    device_t *candidate;
    driver_resource_claim_t *claim;
    int result;
};
static void *inventory_compete(void *opaque) {
    struct inventory_race *race = opaque;
    pthread_barrier_wait(&start);
    race->result =
        race->candidate
            ? device_register(race->candidate)
            : driver_resource_request_checked(&a, &region, &race->claim);
    pthread_barrier_wait(&attempted);
    pthread_barrier_wait(&proceed);
    if (!race->result && race->candidate)
        device_unregister(race->candidate);
    driver_resource_release(race->claim);
    return NULL;
}
static void inventory_concurrency(void) {
    assert(!device_register(&a));
    pthread_barrier_init(&start, NULL, 3);
    pthread_barrier_init(&attempted, NULL, 3);
    pthread_barrier_init(&proceed, NULL, 3);
    for (unsigned i = 0; i < 16; i++) {
        device_t candidate = {.name = "publication-race",
                              .res = &region,
                              .res_count = 1,
                              .references = 1};
        struct inventory_race races[2] = {{0}, {.candidate = &candidate}};
        pthread_t workers[2];
        for (unsigned k = 0; k < 2; k++)
            assert(!pthread_create(&workers[k], NULL, inventory_compete,
                                   &races[k]));
        pthread_barrier_wait(&start);
        pthread_barrier_wait(&attempted);
        assert((!races[0].result && races[1].result == -EBUSY) ||
               (!races[1].result && races[0].result == -EBUSY));
        pthread_barrier_wait(&proceed);
        for (unsigned k = 0; k < 2; k++)
            assert(!pthread_join(workers[k], NULL));
        assert(a.references == 2 && candidate.references == 1);
        device_put(&candidate);
    }
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&attempted);
    pthread_barrier_destroy(&proceed);
    device_unregister(&a);
    assert(a.references == 1);
}
int main(void) {
    driver_core_init();
    int baseline = allocations;
    intervals();
    invalid();
    failures_and_managed();
    draining_unmap();
    concurrency();
    inventory();
    inventory_concurrency();
    lifecycle();
    assert(allocations == baseline && a.references == 1 && b.references == 1 &&
           maps >= unmaps);
    puts("Driver resources: overlap, owner pins, rollback, concurrent claims "
         "and unmap lifetime PASS");
    return 0;
}
