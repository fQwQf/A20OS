/* Production driver core and class layer, with host hardware/allocator stubs. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_registry_test.h"
#include "core/errno.h"

unsigned host_cpu;
int host_trylock_blocked;
struct task_t *proc_current(void) { return NULL; }
void *kmalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t n, size_t size) { return calloc(n, size); }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void kfree(void *p) { free(p); }
void host_log(const char *fmt, ...) { (void)fmt; }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
void *arch_device_map_mmio(uint64_t p, size_t n, uint32_t f)
{ (void)p; (void)n; (void)f; return NULL; }
void arch_device_unmap_mmio(void *p, size_t n) { (void)p; (void)n; }
int firmware_device_prepare(device_t *d) { (void)d; return 0; }
void netlink_uevent_emit(const char *a, const char *s, const char *n, uint64_t t)
{ (void)a; (void)s; (void)n; (void)t; }
__asm__(".global __driver_init_start\n.global __driver_init_end\n"
        "__driver_init_start:\n__driver_init_end:\n");

static unsigned quiesced, removed, released, notified, calls;
static unsigned class_quiesced;
static class_device_t *outstanding;
static int fail_probe;

static void resource_release(void *p)
{
    (void)p;
    assert(removed == quiesced && released + 1 == removed);
    released++;
}
static int probe(device_t *d)
{
    if (fail_probe)
        return -EIO;
    assert(!device_add_resource_action(d, resource_release, d));
    d->drv_priv = d;
    return device_set_devfs_name(d, "native-test");
}
static int matches(device_t *d, const driver_t *driver)
{ (void)d; (void)driver; return 1; }
static void offline(class_device_t *c, void *p)
{
    assert(p == c && !c->online);
    notified++;
}
static void quiesce(device_t *d)
{
    assert(d->drv_priv == d);
    if (d->class_dev) {
        assert(notified == class_quiesced + 1);
        class_quiesced++;
        assert(class_device_call_begin(d->class_dev) == -ENODEV);
        if (outstanding) {
            assert(outstanding == d->class_dev && outstanding->active_calls == 1);
            /* A real driver stops hardware and errors/wakes the blocked I/O
             * here. The call must drain BEFORE remove can release storage. */
            class_device_call_end(outstanding);
            outstanding = NULL;
        }
    }
    quiesced++;
}
static int remove_device(device_t *d)
{
    assert(!outstanding && !d->class_dev && d->drv_priv == d);
    assert(quiesced == removed + 1);
    removed++;
    return 0;
}
static int read_device(device_t *d, void *p, size_t n)
{ (void)d; (void)p; calls++; return (int)n; }

int main(void)
{
    driver_core_init();
    bus_type_t bus = {.name="native-test", .match=matches};
    char_dev_ops_t ops = {.read=read_device};
    driver_t driver = {.name="native-test", .bus=&bus, .probe=probe,
        .remove=remove_device, .quiesce=quiesce, .class_type=DEV_CLASS_CHAR,
        .class_ops=&ops};
    assert(!bus_register(&bus) && !driver_register(&driver));
    device_t a = {.name="native-a", .bus=&bus};
    assert(!device_register(&a) && a.drv == &driver);
    class_device_t *stale = class_device_ref_for_device(&a);
    assert(stale && !class_device_set_offline(stale, offline, stale));
    assert(!class_device_call_begin(stale));
    outstanding = stale;
    device_unregister(&a);
    assert(quiesced == 1 && removed == 1 && released == 1 && notified == 1);
    assert(class_device_call_begin(stale) == -ENODEV && !calls);
    class_device_put(stale);
    /* Re-probe must get a fresh online publication, not inherit offline state. */
    assert(!device_register(&a));
    assert(!class_device_set_offline(a.class_dev, offline, a.class_dev));
    device_t b = {.name="native-b", .bus=&bus};
    assert(!device_register(&b)); /* duplicate class name: successful probe rollback */
    assert(!b.drv && b.probe_error == -EEXIST);
    assert(quiesced == 2 && removed == 2 && released == 2);
    device_unregister(&b);
    device_unregister(&a);
    assert(quiesced == 3 && removed == 3 && released == 3 && notified == 2);
    assert(!driver_unregister(&driver));
    driver.class_type = DEV_CLASS_NONE;
    assert(!driver_register(&driver));
    assert(!device_register(&b) && b.drv == &driver && !b.class_dev);
    device_unregister(&b);
    assert(quiesced == 4 && removed == 4 && released == 4);
    fail_probe = 1;
    assert(!device_register(&b) && !b.drv && b.probe_error == -EIO);
    device_unregister(&b);
    assert(quiesced == 4); /* Failed probe owns its own rollback. */
    assert(!driver_registry_capacity_test_run());
    assert(!driver_unregister(&driver));
    bus_unregister(&bus);
    puts("native quiesce: PASS");
    return 0;
}
