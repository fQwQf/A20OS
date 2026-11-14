/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/runtime_test.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/fence.h"
#include "drivers/core/queue.h"
#include "drivers/core/protocol.h"
#include "drivers/net/interface.h"
#include "drivers/core/workqueue.h"
#include "drivers/core/firmware.h"
#include "drivers/core/amd_iommu.h"
#include "drivers/core/dma_mapping.h"
#include "drivers/core/memory.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/usb/usb.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/gpu/drm.h"
#include "mm/frame.h"
#include "mm/vmo.h"
#include "mm/vm.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "core/bootargs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/fcntl.h"
#include "core/poll.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "proc/proc.h"

#define CHECK(test) do { if (!(test)) \
    panic("DRIVER_RUNTIME: FAIL line=%d: %s", __LINE__, #test); } while (0)

struct runtime_case {
    struct driver_workqueue *queue;
    struct driver_work *work;
    struct driver_fence *entered, *release, *done;
    unsigned count;
    int error;
};

static uint64_t deadline(void) { return clock_get_ticks() + MS_TO_TICKS(2000); }

static void delayed_callback(void *opaque)
{
    struct runtime_case *test = opaque;
    test->count++;
    CHECK(!driver_fence_signal(test->done, 0));
}

static void requeue_callback(void *opaque)
{
    struct runtime_case *test = opaque;
    if (++test->count == 1) {
        CHECK(driver_work_schedule(test->work, 0) == 1);
    } else {
        CHECK(driver_work_cancel_sync(test->work, 0) == -EDEADLK);
        CHECK(!driver_fence_signal(test->done, 0));
    }
}

static void blocked_callback(void *opaque)
{
    struct runtime_case *test = opaque;
    CHECK(!driver_fence_signal(test->entered, 0));
    test->error = driver_fence_wait(test->release, deadline(), false);
    test->count++;
}

static void exercise(void)
{
    struct runtime_case test = {0};
    test.queue = driver_workqueue_create();
    test.done = driver_fence_create();
    CHECK(test.queue && test.done);
    test.work = driver_work_create(test.queue, delayed_callback, &test);
    CHECK(test.work);
    uint64_t due = clock_get_ticks() + MS_TO_TICKS(15);
    CHECK(driver_work_schedule(test.work, due) == 1);
    CHECK(driver_work_schedule(test.work, 0) == 0);
    CHECK(!driver_fence_wait(test.done, deadline(), false));
    CHECK(clock_get_ticks() >= due);
    CHECK(!driver_workqueue_flush(test.queue, deadline()));
    CHECK(test.count == 1);
    driver_fence_put(test.done);
    test.done = driver_fence_create();
    CHECK(test.done);
    CHECK(driver_work_schedule(test.work, clock_get_ticks() + MS_TO_TICKS(10000)) == 1);
    due = clock_get_ticks() + MS_TO_TICKS(15);
    CHECK(driver_work_reschedule(test.work, due) == 0);
    CHECK(!driver_fence_wait(test.done, deadline(), false));
    CHECK(!driver_workqueue_flush(test.queue, deadline()));
    CHECK(test.count == 2 && clock_get_ticks() >= due);
    CHECK(driver_work_schedule(test.work, clock_get_ticks() + MS_TO_TICKS(10000)) == 1);
    CHECK(!driver_work_cancel_sync(test.work, deadline()));
    CHECK(!driver_workqueue_flush(test.queue, deadline()) && test.count == 2);
    driver_work_put(test.work);
    driver_fence_put(test.done);

    test.count = 0;
    test.done = driver_fence_create();
    test.work = driver_work_create(test.queue, requeue_callback, &test);
    CHECK(test.done && test.work);
    CHECK(driver_work_schedule(test.work, 0) == 1);
    CHECK(!driver_fence_wait(test.done, deadline(), false));
    CHECK(!driver_workqueue_flush(test.queue, deadline()) && test.count == 2);
    driver_work_put(test.work);
    driver_fence_put(test.done);

    test.count = 0;
    test.entered = driver_fence_create();
    test.release = driver_fence_create();
    test.work = driver_work_create(test.queue, blocked_callback, &test);
    CHECK(test.entered && test.release && test.work);
    CHECK(driver_work_schedule(test.work, 0) == 1);
    CHECK(!driver_fence_wait(test.entered, deadline(), false));
    CHECK(driver_workqueue_destroy(&test.queue,
          clock_get_ticks() + MS_TO_TICKS(5)) == -ETIMEDOUT);
    CHECK(test.queue && driver_work_schedule(test.work, 0) == -ESHUTDOWN);
    CHECK(!driver_fence_signal(test.release, 0));
    CHECK(!driver_workqueue_destroy(&test.queue, deadline()) && !test.queue);
    CHECK(test.count == 1 && !test.error);
    CHECK(driver_work_schedule(test.work, 0) == -ESHUTDOWN);
    driver_work_put(test.work);
    driver_fence_put(test.entered);
    driver_fence_put(test.release);
}

/* Actual native workers/fences and lifecycle; delayed work models a device
 * completion producer. VirtIO GPU exercises the real transport separately. */
struct request_case {
    struct driver_queue *commands;
    struct driver_workqueue *worker;
    struct driver_work *completion;
    struct driver_request *inflight[4];
    spinlock_t lock;
    unsigned count, issued;
    int stop_error;
};
static void request_completion(void *context)
{
    struct request_case *test = context;
    struct driver_request *requests[4];
    uint64_t flags = spin_lock_irqsave(&test->lock);
    unsigned count = test->count;
    for (unsigned i = 0; i < count; i++) requests[i] = test->inflight[i];
    test->count = 0;
    spin_unlock_irqrestore(&test->lock, flags);
    /* Hardware may finish independent requests in reverse order. */
    while (count) CHECK(!driver_request_complete(requests[--count], 0));
}
static void request_issue(void *context, struct driver_request *request)
{
    struct request_case *test = context;
    uint64_t flags = spin_lock_irqsave(&test->lock);
    CHECK(test->count < 4);
    test->inflight[test->count++] = request;
    test->issued++;
    spin_unlock_irqrestore(&test->lock, flags);
    CHECK(driver_work_schedule(test->completion, clock_get_ticks() + MS_TO_TICKS(5)) >= 0);
}
static int request_stop(void *context)
{
    struct request_case *test = context;
    if (test->stop_error) return test->stop_error;
    return driver_work_cancel_sync(test->completion, deadline());
}
static int request_ready(device_t *dev)
{ return ((struct request_case *)dev->plat_data)->commands != NULL; }
static const struct { int (*ready)(device_t *); } request_protocol = {request_ready};
static int request_probe(device_t *dev)
{
    struct request_case *test = dev->plat_data;
    const struct driver_protocol_descriptor protocol = {
        .info = {0x800000000000a200ULL, 1, sizeof(request_protocol), 0}, .ops = &request_protocol,
    };
    int result = device_add_protocol(dev, &protocol);
    if (result) return result;
    const struct driver_queue_limits limits = {3, 2, 4, 12};
    const struct driver_queue_ops ops = {.size = sizeof(ops), .issue = request_issue, .stop = request_stop};
    return driver_queue_create(dev, &limits, &ops, test, &test->commands);
}
static int request_remove(device_t *dev)
{
    struct request_case *test = dev->plat_data;
    driver_queue_put(test->commands); test->commands = NULL;
    return 0;
}
static int request_match(device_t *dev) { return !strcmp(dev->name, "native-request-selftest"); }
static void request_release_call(void *context, int status)
{
    CHECK(status == -ESHUTDOWN);
    class_device_call_end(context);
}
static int request_unregister(driver_t *driver)
{
    uint64_t end = deadline();
    int result;
    do { result = driver_unregister(driver); if (result == -EBUSY) proc_yield(); }
    while (result == -EBUSY && clock_get_ticks() < end);
    return result;
}
static void exercise_requests(void)
{
    struct request_case test = {0}; spin_init(&test.lock);
    test.worker = driver_workqueue_create(); CHECK(test.worker);
    test.completion = driver_work_create(test.worker, request_completion, &test); CHECK(test.completion);
    driver_t driver = {.name = "native-request-selftest", .match = request_match,
        .probe = request_probe, .remove = request_remove};
    device_t dev = {.name = "native-request-selftest", .plat_data = &test};
    CHECK(!driver_register(&driver) && !device_register(&dev) && test.commands);
    CHECK(!driver_queue_start(test.commands, test.worker));
    struct driver_request *requests[3];
    for (unsigned i = 0; i < 3; i++) {
        struct driver_request_description description = {.command = &i, .command_size = sizeof(i)};
        CHECK(!driver_request_create(test.commands, &description, &requests[i]));
        CHECK(!driver_request_submit(requests[i]));
    }
    for (unsigned i = 0; i < 3; i++) {
        CHECK(!driver_fence_wait(driver_request_fence(requests[i]), deadline(), false));
        driver_request_put(requests[i]);
    }
    CHECK(test.issued == 3);
    class_device_t *endpoint = class_device_ref_for_device(&dev); CHECK(endpoint);
    CHECK(!class_device_call_begin(endpoint));
    struct driver_fence *dependency = driver_fence_create(); CHECK(dependency);
    struct driver_request_description description = {.dependency = dependency};
    struct driver_request *pending; CHECK(!driver_request_create(test.commands, &description, &pending));
    CHECK(!driver_request_submit(pending));
    struct driver_fence_watch *watch;
    CHECK(!driver_fence_watch_create(driver_request_fence(pending), request_release_call, endpoint, &watch));
    test.stop_error = -ETIMEDOUT;
    CHECK(request_unregister(&driver) == -ETIMEDOUT && dev.drv == &driver && test.commands);
    CHECK(!driver_fence_status(driver_request_fence(pending)));
    test.stop_error = 0;
    CHECK(!request_unregister(&driver) && !test.commands);
    CHECK(driver_fence_status(driver_request_fence(pending)) == -ESHUTDOWN);
    CHECK(!driver_fence_watch_try_remove(&watch));
    driver_request_put(pending); driver_fence_put(dependency);
    class_device_put(endpoint);
    CHECK(!device_unregister(&dev)); device_put(&dev);
    driver_work_put(test.completion);
    CHECK(!driver_workqueue_destroy(&test.worker, deadline()));
}

struct notification_case {
    struct driver_work *work;
    struct driver_fence *done;
    unsigned notified, cancelled;
    int status;
};

static void notification_work(void *opaque)
{
    struct notification_case *test = opaque;
    CHECK(!driver_fence_signal(test->done, 0));
}

static void fence_notification(void *opaque, int status)
{
    struct notification_case *test = opaque;
    test->status = status;
    test->notified++;
    CHECK(driver_work_schedule(test->work, 0) == 1);
}

static void cancelled_notification(void *opaque, int status)
{
    (void)status;
    ((struct notification_case *)opaque)->cancelled++;
}

static void exercise_fence_notifications(void)
{
    struct driver_workqueue *queue = driver_workqueue_create();
    CHECK(queue);
    for (unsigned failed = 0; failed < 2; failed++) {
        struct notification_case test = {0};
        struct driver_fence *source = driver_fence_create();
        test.done = driver_fence_create();
        test.work = driver_work_create(queue, notification_work, &test);
        CHECK(source && test.done && test.work);
        struct driver_fence_watch *watch, *cancelled;
        CHECK(!driver_fence_watch_create(source, cancelled_notification, &test, &cancelled));
        CHECK(!driver_fence_watch_create(source, fence_notification, &test, &watch));
        CHECK(!driver_fence_watch_try_remove(&cancelled) && !cancelled);
        CHECK(!driver_fence_status(source) && !test.notified);
        CHECK(!driver_fence_signal(source, failed ? -EIO : 0));
        CHECK(test.notified == 1 && !test.cancelled && test.status == (failed ? -EIO : 1));
        CHECK(!driver_fence_watch_try_remove(&watch) && !watch);
        CHECK(driver_fence_watch_create(source, fence_notification, &test, &watch) == -EALREADY);
        CHECK(!watch);
        CHECK(!driver_fence_wait(test.done, deadline(), false));
        CHECK(!driver_workqueue_flush(queue, deadline()));
        driver_work_put(test.work);
        driver_fence_put(test.done);
        driver_fence_put(source);
    }
    CHECK(!driver_workqueue_destroy(&queue, deadline()) && !queue);
}

static void exercise_firmware(void)
{
    const char bytes[] = {'A', '2', '0', '\0', 1, 2, 3};
    const char *root = "/driver-firmware-test";
    const char *subdir = "/driver-firmware-test/vendor";
    const char *path = "/driver-firmware-test/vendor/gpu.bin";
    const char *alias = "/driver-firmware-test/alias";
    struct driver_firmware *image;
    CHECK(!driver_firmware_from_memory(bytes, sizeof(bytes), sizeof(bytes), &image));
    CHECK(driver_firmware_size(image) == sizeof(bytes));
    CHECK(!memcmp(driver_firmware_data(image), bytes, sizeof(bytes)));
    driver_firmware_put(image);
    CHECK(!vfs_mkdir(root, 0755) && !vfs_mkdir(subdir, 0755));
    int fd = vfs_open(path, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0);
    CHECK(vfs_write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(!vfs_close(fd));
    fd = vfs_open(root, O_RDONLY | O_DIRECTORY, 0);
    CHECK(fd >= 0);
    vfile_t *directory = vfs_get_file_ref(fd);
    struct driver_firmware_source *source;
    CHECK(directory && !driver_firmware_source_create_vfs(directory, &source));
    vfs_put_file(directory);
    CHECK(!vfs_close(fd)); /* Loads must outlive the caller's descriptor. */
    CHECK(driver_firmware_load(source, "../outside", 128, &image) == -EINVAL);
    CHECK(driver_firmware_load(source, "missing", 128, &image) == -ENOENT);
    CHECK(!vfs_symlink("vendor/gpu.bin", alias));
    CHECK(driver_firmware_load(source, "alias", 128, &image) == -ELOOP);
    CHECK(driver_firmware_load(source, "vendor/gpu.bin", sizeof(bytes)-1, &image) == -EFBIG);
    CHECK(!driver_firmware_load(source, "vendor/gpu.bin", sizeof(bytes), &image));
    driver_firmware_source_put(source);
    CHECK(!vfs_unlink(alias) && !vfs_unlink(path));
    CHECK(!vfs_rmdir(subdir) && !vfs_rmdir(root));
    /* The image owns its bytes independently of the source and inode. */
    CHECK(driver_firmware_size(image) == sizeof(bytes));
    CHECK(!memcmp(driver_firmware_data(image), bytes, sizeof(bytes)));
    driver_firmware_put(image);
}

static bool boot_flag(const char *args, const char *key)
{
    const char *at = args;
    while (at && (at = strstr(at, key))) {
        char next = at[strlen(key)];
        if ((at == args || at[-1] == ' ' || at[-1] == '\t') &&
            (!next || next == ' ' || next == '\t'))
            return true;
        at++;
    }
    return false;
}

static int resource_test_match(device_t *dev, const driver_t *driver) {
    (void)dev;
    (void)driver;
    return 0;
}
static void exercise_resources(void) {
    /* Metadata-only claims outside usable device/RAM addresses. No MMIO map,
     * port access or hardware reservation is performed by this test. */
    resource_t range = {
        .type = RES_MMIO, .start = ~(uint64_t)0 - 0x3fff, .end = ~(uint64_t)0};
    device_t a = {.name = "resource-selftest-a",
                  .res = &range,
                  .res_count = 1,
                  .references = 1};
    device_t b = {.name = "resource-selftest-b", .references = 1};
    driver_resource_claim_t *first, *second, *conflict;
    CHECK(!driver_resource_request(&a, &range, &first) && a.references == 2);
    CHECK(driver_resource_request(&b, &range, &conflict) == -EBUSY &&
          !conflict);
    CHECK(!driver_resource_request(&a, &range, &second) && a.references == 3);
    driver_resource_release(first);
    CHECK(driver_resource_request(&b, &range, &conflict) == -EBUSY &&
          !conflict);
    driver_resource_release(second);
    CHECK(a.references == 1 && !driver_resource_request(&b, &range, &first));
    driver_resource_release(first);
    CHECK(!device_claim_resource(&a, &range) && a.references == 2);
    CHECK(driver_resource_request(&b, &range, &conflict) == -EBUSY &&
          !conflict);
    device_release_resources(&a);
    CHECK(!a.resource_actions && a.references == 1);
    CHECK(!driver_resource_request(&b, &range, &first));
    driver_resource_release(first);
    CHECK(b.references == 1);
    bus_type_t bus = {.name = "resource-selftest",
                      .match = resource_test_match};
    a.bus = b.bus = &bus;
    b.res = &range;
    b.res_count = 1;
    CHECK(!bus_register(&bus) && !device_register(&a) && !device_register(&b));
    CHECK(!a.drv && !b.drv);
    CHECK(driver_resource_request_checked(&a, &range, &conflict) == -EBUSY &&
          !conflict);
    device_unregister(&b);
    CHECK(!driver_resource_request_checked(&a, &range, &first));
    CHECK(device_register(&b) == -EBUSY && b.references == 1);
    device_unregister(&a);
    CHECK(device_register(&b) ==
          -EBUSY); /* Checked lease outlives registration. */
    driver_resource_release(first);
    CHECK(!device_register(&b));
    device_unregister(&b);
    bus_unregister(&bus);
    a.bus = b.bus = NULL;
    driver_memory_range_t storage = {range.start, range.end};
    driver_memory_reservation_t *reservation;
    CHECK(!driver_memory_reserve(&storage, 1, &reservation));
    CHECK(driver_resource_request(&a, &range, &conflict) == -EBUSY && !conflict);
    CHECK(device_register(&a) == -EBUSY);
    resource_t ram_description = range;
    ram_description.type = RES_MEM;
    a.res = &ram_description;
    CHECK(!device_register(&a)); /* Storage descriptions may coexist. */
    CHECK(driver_resource_request_checked(&a, &range, &conflict) == -EBUSY && !conflict);
    device_unregister(&a);
    driver_memory_release(reservation);
    CHECK(!driver_resource_request(&a, &range, &first));
    driver_resource_release(first);
    CHECK(driver_memory_protect_boot() == -EALREADY);
    CHECK(pfa.nr_ranges > 0 && pfa.ranges[0].end > pfa.ranges[0].base);
    resource_t real_ram = {.type = RES_MMIO, .start = pfa.ranges[0].base,
                           .end = pfa.ranges[0].end - 1};
    CHECK(driver_resource_request(&a, &real_ram, &conflict) == -EBUSY && !conflict);
    CHECK(a.references == 1 && b.references == 1);
    device_put(&a);
    device_put(&b);
}

static void exercise_pci_scope(void) {
    /* Explicit QEMU gate: one EDU endpoint on bus 1 behind a root port.
     * Only query identity and topology; never enable/assign its BAR or DMA. */
    driver_discovery_begin();
    device_t *client = NULL, *anchor = NULL;
    CHECK(!pci_device_find_get(0, 0x100, &client));
    CHECK(pci_device_id(client) == 0x123411e8);
    CHECK(!pci_device_scope_get(client, 0, 0, 255, &anchor));
    CHECK(anchor != client && (pci_class_code(anchor) >> 8) == 0x0604);
    device_put(anchor);
    CHECK(!pci_device_scope_get(client, 1, 0, 0, &anchor) && anchor == client);
    device_put(anchor);
    CHECK(pci_device_scope_get(client, 1, 1, 255, &anchor) == -ENODEV && !anchor);
    device_put(client);
    driver_discovery_end();
}

static void exercise_amdvi_route(bool reserve, bool policy_test) {
    /* Explicit q35/root-port/EDU test. Resolve policy and optionally reserve
     * software keys; never enable bus mastering or program device tables. */
    driver_discovery_begin();
    device_t *client = NULL;
    CHECK(!pci_device_find_get(0, 0x100, &client));
    CHECK(pci_device_id(client) == 0x123411e8);
    amd_iommu_route_t *route = NULL;
    CHECK(!amd_iommu_route_acquire(client, &route));
    amd_iommu_route_t *peer = NULL;
    if (reserve)
        CHECK(!amd_iommu_route_acquire(client, &peer));
    device_put(client); /* The route independently retains the client. */
    amd_iommu_route_info_t info;
    CHECK(!amd_iommu_route_validate(route, &info));
    CHECK(info.requester == 0x100 && info.dma_requester == 0x100 && !info.alias);
    CHECK(info.unit.register_size && info.unit.base);
    if (reserve) {
        uint16_t ids[2] = {0xffff, 0xffff};
        size_t count = 0;
        CHECK(amd_iommu_route_requesters(route, NULL, 0, &count) == -ENOSPC && count == 1);
        CHECK(!amd_iommu_route_requesters(route, ids, 2, &count));
        CHECK(count == 1 && ids[0] == 0x100 && ids[1] == 0xffff);
    }
    struct driver_dma_policy *policy = NULL;
    if (policy_test) {
        CHECK(!info.memory_count); /* QEMU fixture has no IVMD blocks. */
        CHECK(!amd_iommu_route_policy_create(route, &policy));
    }
    acpi_ivrs_memory_t memory;
    for (size_t i = 0; i < info.memory_count; i++)
        CHECK(!amd_iommu_route_memory(route, i, &memory));
    CHECK(amd_iommu_route_memory(route, info.memory_count, &memory) == -ENODEV);
    CHECK(!memory.start && !memory.length);
    if (reserve) {
        CHECK(!amd_iommu_route_reserve(route));
        CHECK(amd_iommu_route_reserve(route) == -EALREADY);
        CHECK(amd_iommu_route_reserve(peer) == -EBUSY);
        amd_iommu_route_release(route);
        route = peer;
        CHECK(!amd_iommu_route_reserve(route));
    }
    amd_iommu_route_release(route);
    if (policy_test) {
        bool identity = true;
        CHECK(!driver_dma_policy_check(policy, 0, 1, DRIVER_DMA_BIDIRECTIONAL, &identity));
        CHECK(!identity);
        driver_dma_policy_put(policy);
    }
    driver_discovery_end();
    kinfo("AMDVI_ROUTE: PASS route=1 policy=1 scope=1\n");
    if (reserve)
        kinfo("AMDVI_REQUESTER: PASS reserve=1 conflict=1 release=1\n");
    if (reserve)
        kinfo("AMDVI_PEERS: PASS query=1\n");
    if (policy_test)
        kinfo("AMDVI_POLICY: PASS create=1 retained=1\n");
}

/* Explicit embedded QEMU q35/xHCI/hub fixture, never a normal boot action. */
static device_t *xhci_input_child(device_t *controller) {
    for (int index = 0;; index++) {
        device_t *dev = device_find_get_by_class(DEV_CLASS_INPUT, index);
        if (!dev) return NULL;
        device_t *parent = dev->parent;
        for (unsigned depth = 0; parent && depth < 16; depth++, parent = parent->parent)
            if (parent == controller) return dev;
        device_put(dev);
    }
}
/* Embedded fixture: withdraw interface drivers with the root HCD alive. */
static void exercise_usb_interfaces(device_t *controller) {
    device_t *input = xhci_input_child(controller);
    CHECK(input && input->drv && input->plat_data);
    driver_t *hid = input->drv;
    driver_t *root = controller->drv;
    usb_interface_t *iface = input->plat_data;
    usb_device_t *protocol = iface->dev;
    uint8_t slot = protocol->slot;
    class_device_t *old_class = class_device_ref_for_device(input);
    CHECK(old_class);
    CHECK(!driver_unregister(hid));
    CHECK(controller->drv == root && controller->drv_priv);
    CHECK(device_is_registered(input) && !input->drv && !input->drv_priv);
    CHECK(input->plat_data == iface && iface->dev == protocol && protocol->slot == slot);
    CHECK(class_device_call_begin(old_class) == -ENODEV);
    CHECK(!driver_register(hid) && input->drv == hid && input->drv_priv);
    class_device_t *new_class = class_device_ref_for_device(input);
    CHECK(new_class && new_class != old_class);
    class_device_put(old_class);
    device_t *hub = device_get(input->parent);
    CHECK(hub && hub->drv && hub->plat_data);
    CHECK(((usb_interface_t *)hub->plat_data)->interface_class == USB_CLASS_HUB);
    driver_t *hub_driver = hub->drv;
    CHECK(!driver_unregister(hub_driver));
    CHECK(controller->drv == root && controller->drv_priv);
    CHECK(device_is_registered(hub) && !hub->drv && !hub->drv_priv);
    CHECK(!device_is_registered(input) && !input->plat_data);
    CHECK(class_device_call_begin(new_class) == -ENODEV);
    CHECK(!driver_register(hub_driver) && hub->drv == hub_driver);
    usb_core_scan();
    device_t *fresh = xhci_input_child(controller);
    CHECK(fresh && fresh != input && fresh->drv == hid);
    device_put(fresh);
    device_put(hub);
    class_device_put(new_class);
    device_put(input);
    kinfo("DRIVER_USB_INTERFACE: PASS hid=1 hub=1 siblings=1 rebind=1\n");
}

static int gpu_test_match(device_t *dev, const driver_t *driver) {
    return dev->plat_data == driver;
}
static int gpu_test_info(device_t *dev, uint32_t *width, uint32_t *height, uint32_t *bpp) {
    (void)dev; *width = 9; *height = 7; *bpp = 32; return 0;
}
static int gpu_test_capabilities(device_t *dev, struct gpu_capabilities *out) {
    (void)dev;out->flags = GPU_CAP_SCANOUT;return 0;
}
static int gpu_test_probe(device_t *dev) { return gpu_device_register(dev); }
static int gpu_test_remove(device_t *dev) { gpu_device_unregister(dev); return 0; }
/* Explicit role-policy fixture, with no hardware GPU in its instance. The
 * declared rendering roles are synthetic; no GPU commands execute here. */
struct gpu_caps_fixture { driver_t *driver; struct gpu_capabilities capabilities; };
static int gpu_caps_match(device_t *dev, const driver_t *driver) {
    return ((struct gpu_caps_fixture *)dev->plat_data)->driver == driver;
}
static int gpu_caps_query(device_t *dev, struct gpu_capabilities *out) {
    *out = ((struct gpu_caps_fixture *)dev->plat_data)->capabilities;
    return 0;
}
static void gpu_caps_expect(device_t *expected) {
    class_device_t *publication = gpu_device_default_get();
    if (!expected) { CHECK(!publication); return; }
    CHECK(publication && publication->dev == expected);
    struct gpu_device_access access;
    struct gpu_capabilities capabilities;
    CHECK(!gpu_device_access_begin(publication, &access));
    CHECK(!gpu_device_capabilities_get(&access, &capabilities) &&
          capabilities.flags == ((struct gpu_caps_fixture *)expected->plat_data)->capabilities.flags);
    CHECK(!access.ops->get_capset && !access.ops->submit_3d);
    gpu_device_access_end(&access);class_device_put(publication);
}
static void exercise_gpu_capabilities(void) {
    gpu_caps_expect(NULL);
    bus_type_t bus = {.name = "gpu-caps-selftest", .match = gpu_caps_match};
    const gpu_dev_ops_t ops = {.get_info = gpu_test_info, .get_capabilities = gpu_caps_query};
    driver_t driver = {.name = "gpu-caps-selftest", .bus = &bus,
        .probe = gpu_test_probe, .remove = gpu_test_remove,
        .class_type = DEV_CLASS_DISPLAY, .class_ops = &ops};
    uint64_t roles[] = {
        GPU_CAP_SCANOUT | GPU_CAP_BOOT_SCANOUT, GPU_CAP_RENDER,
        GPU_CAP_SCANOUT, GPU_CAP_SCANOUT,
        GPU_CAP_SCANOUT | GPU_CAP_RENDER | GPU_CAP_BOOT_SCANOUT,
        GPU_CAP_SCANOUT | GPU_CAP_RENDER, GPU_CAP_SCANOUT | GPU_CAP_RENDER,
        GPU_CAP_RAM_SCANOUT, /* Invalid role/callback combination. */
    };
    const char *names[] = {"gpu-caps-boot", "gpu-caps-headless", "gpu-caps-managed",
        "gpu-caps-managed-peer", "gpu-caps-boot-render", "gpu-caps-render",
        "gpu-caps-render-peer", "gpu-caps-invalid"};
    struct gpu_caps_fixture cases[8];
    device_t devices[8];
    CHECK(!bus_register(&bus) && !driver_register(&driver));
    for (unsigned i = 0; i < 8; i++) {
        cases[i] = (struct gpu_caps_fixture){&driver, {roles[i]}};
        devices[i] = (device_t){.name = names[i], .bus = &bus,
            .plat_data = &cases[i], .references = 1};
        CHECK(!device_register(&devices[i]));
        if (i == 7) CHECK(!devices[i].drv && !devices[i].class_dev &&
                          devices[i].probe_error == -EINVAL);
        else CHECK(devices[i].drv == &driver);
        unsigned expected = i < 2 ? 0 : i < 4 ? 2 : i == 4 ? 4 : 5;
        gpu_caps_expect(&devices[expected]);
    }
    /* Re-election must pass a firmware renderer to find a managed renderer,
     * then recover every lower priority without selecting the headless GPU. */
    CHECK(!device_unregister(&devices[5]));gpu_caps_expect(&devices[6]);
    CHECK(!device_unregister(&devices[6]));gpu_caps_expect(&devices[4]);
    CHECK(!device_unregister(&devices[4]));
    CHECK(!device_unregister(&devices[2]));gpu_caps_expect(&devices[3]);
    CHECK(!device_unregister(&devices[3]));gpu_caps_expect(&devices[0]);
    CHECK(!device_unregister(&devices[0]));gpu_caps_expect(NULL);
    CHECK(!device_unregister(&devices[1]) && !device_unregister(&devices[7]));
    for (unsigned i = 0; i < 8; i++) {
        CHECK(devices[i].references == 1);
        device_put(&devices[i]);CHECK(!devices[i].references);
    }
    CHECK(!driver_unregister(&driver));bus_unregister(&bus);
}

static void exercise_gpu_access(void) {
    bus_type_t bus = {.name = "gpu-access-selftest", .match = gpu_test_match};
    const gpu_dev_ops_t ops = {.get_info = gpu_test_info, .get_capabilities = gpu_test_capabilities};
    driver_t driver = {.name = "gpu-access-selftest", .bus = &bus,
        .probe = gpu_test_probe, .remove = gpu_test_remove,
        .class_type = DEV_CLASS_DISPLAY, .class_ops = &ops};
    device_t device = {.name = "gpu-access-selftest", .bus = &bus,
        .plat_data = &driver, .references = 1};
    CHECK(!bus_register(&bus) && !driver_register(&driver) && !device_register(&device));
    CHECK(device.drv == &driver);
    class_device_t *publication = class_device_ref_for_device(&device);
    struct gpu_device_access access;
    CHECK(publication && !gpu_device_access_begin(publication, &access));
    CHECK(access.device == &device && access.ops == &ops && publication->active_calls == 1);
    uint32_t width, height, bpp;
    CHECK(!access.ops->get_info(access.device, &width, &height, &bpp));
    CHECK(width == 9 && height == 7 && bpp == 32);
    struct gpu_capabilities capabilities;
    CHECK(!gpu_device_capabilities_get(&access, &capabilities) && capabilities.flags == GPU_CAP_SCANOUT);
    gpu_device_access_end(&access);
    capabilities.flags = GPU_CAP_KNOWN;
    CHECK(gpu_device_capabilities_get(&access, &capabilities) == -ENODEV && !capabilities.flags);
    CHECK(!access.device && !access.ops && !access.publication);
    CHECK(!gpu_device_default_begin(&access)); /* May select an existing real GPU. */
    CHECK(access.publication->dev == access.device && access.ops);
    gpu_device_access_end(&access);
    CHECK(!driver_unregister(&driver));
    CHECK(gpu_device_access_begin(publication, &access) == -ENODEV && !access.ops);
    CHECK(!device_unregister(&device));device_put(&device);
    CHECK(device.references == 1); /* Retained offline publication. */
    class_device_put(publication);CHECK(!device.references);
    bus_unregister(&bus);
}

#ifndef CONFIG_NOMMU
static int scanout_test_probe(device_t *dev) {
    struct vmo *memory;
    int ret = vmo_create_contiguous(2 * PAGE_SIZE + 33, &memory);
    if (ret) return ret;
    dev->drv_priv = memory;
    ret = gpu_device_register(dev);
    if (ret) { dev->drv_priv = NULL; vmo_release(memory); }
    return ret;
}
static int scanout_test_get(device_t *dev, struct gpu_scanout *out) {
    struct vmo *memory = dev->drv_priv;
    if (!memory) return -ENODEV;
    vmo_ref(memory);
    *out = (struct gpu_scanout){memory, 0, 2 * PAGE_SIZE + 33};
    return 0;
}
static int scanout_test_capabilities(device_t *dev, struct gpu_capabilities *out) {
    (void)dev;out->flags = GPU_CAP_SCANOUT | GPU_CAP_RAM_SCANOUT;return 0;
}
static int scanout_test_remove(device_t *dev) {
    gpu_device_unregister(dev);
    struct vmo *memory = dev->drv_priv;
    dev->drv_priv = NULL;
    vmo_release(memory);
    return 0;
}
static void exercise_gpu_scanout(void) {
    bus_type_t bus = {.name = "scanout-vmo-selftest", .match = gpu_test_match};
    const gpu_dev_ops_t ops = {.get_info = gpu_test_info, .get_scanout = scanout_test_get, .get_capabilities = scanout_test_capabilities};
    driver_t driver = {.name = "scanout-vmo-selftest", .bus = &bus,
        .probe = scanout_test_probe, .remove = scanout_test_remove,
        .class_type = DEV_CLASS_DISPLAY, .class_ops = &ops};
    device_t device = {.name = "scanout-vmo-selftest", .bus = &bus,
        .plat_data = &driver, .references = 1};
    CHECK(!bus_register(&bus) && !driver_register(&driver) && !device_register(&device));
    CHECK(device.drv == &driver);
    class_device_t *publication = class_device_ref_for_device(&device);
    struct gpu_device_access access;
    CHECK(publication && !gpu_device_access_begin(publication, &access));
    struct gpu_scanout scanout;
    CHECK(!gpu_device_scanout_get(&access, &scanout));
    gpu_device_access_end(&access); /* The snapshot outlives this call. */
    mm_struct_t *mm = mm_create();
    CHECK(mm);
    vaddr_t va = mm_mmap_vmo_cap(mm, 0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, scanout.memory, 0, PROT_READ | PROT_WRITE);
    CHECK(!mm_addr_is_error(va));
    pfn_t first = vmo_peek_page(scanout.memory, 0);
    ((uint8_t *)pfn_to_virt(first))[0] = 0x5a;
    gpu_scanout_put(&scanout);
    CHECK(!driver_unregister(&driver) && !device.drv_priv);
    CHECK(gpu_device_access_begin(publication, &access) == -ENODEV);
    CHECK(!device_unregister(&device));device_put(&device);class_device_put(publication);
    CHECK(!device.references);bus_unregister(&bus);
    /* Only native VMA references remain. Split and clone unpopulated VMO
     * views here; the userspace fbdev test covers resident fault/fork too. */
    CHECK(!mm_mprotect(mm, va + PAGE_SIZE, PAGE_SIZE, PROT_READ));
    mm_struct_t *child = mm_fork(mm);
    CHECK(child);
    mm_destroy(mm);
    CHECK(!mm_munmap(child, va, 2 * PAGE_SIZE));
    mm_seg_t *tail = mm_seg_find(child, va + 2 * PAGE_SIZE);
    CHECK(tail && (tail->vm_flags & VM_VMO) && tail->backing_offset == 2 * PAGE_SIZE);
    uint64_t physical;size_t extent;
    CHECK(!vmo_contiguous_info(tail->vmo, &physical, &extent) && extent == 3 * PAGE_SIZE);
    CHECK(physical == pfn_to_phys(first) && ((uint8_t *)pfn_to_virt(first))[0] == 0x5a);
    CHECK(vmo_resize(tail->vmo, 0) == -EOPNOTSUPP);
    pfn_t pages[3];
    CHECK(!vmo_pin_pages(tail->vmo, 0, 3, pages));
    mm_destroy(child); /* Pins now outlive every VMO and device reference. */
    CHECK(pages[0] == first && ((uint8_t *)pfn_to_virt(first))[0] == 0x5a);
    frame_put_many(pages, 3);
}

/* Explicit embedded virtio-gpu fixture: real reset/readback, IRQ teardown,
 * native unbind/rebind and VMA-owned RAM. No userspace PTE faults here. */
static void exercise_gpu_stop(void) {
    device_t *gpu = NULL;
    for (int index = 0;; index++) {
        device_t *candidate = device_find_get_by_class(DEV_CLASS_DISPLAY, index);
        if (!candidate) break;
        if (candidate->drv && !strcmp(candidate->drv->name, "virtio-gpu")) {
            gpu = candidate; break;
        }
        device_put(candidate);
    }
    CHECK(gpu);
    class_device_t *old = class_device_ref_for_device(gpu);
    struct gpu_device_access access;
    CHECK(old && !gpu_device_access_begin(old, &access));
    driver_t *driver = gpu->drv; /* Embedded static code, no module unload. */
    struct gpu_scanout scanout;
    CHECK(!gpu_device_scanout_get(&access, &scanout) && scanout.size >= PAGE_SIZE);
    struct vmo *memory = scanout.memory;
    mm_struct_t *mm = mm_create();
    CHECK(mm);
    vaddr_t va = mm_mmap_vmo_cap(mm, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, memory, scanout.offset, PROT_READ | PROT_WRITE);
    CHECK(!mm_addr_is_error(va));
    pfn_t page = vmo_peek_page(memory, scanout.offset / PAGE_SIZE);
    CHECK(page != PFN_NONE);
    ((uint8_t *)pfn_to_virt(page))[0] = 0x69;
    gpu_scanout_put(&scanout);
    gpu_device_access_end(&access);
    CHECK(!driver_unregister(driver) && !gpu->drv && !gpu->drv_priv);
    CHECK(class_device_binding_stopped(old));
    CHECK(gpu_device_access_begin(old, &access) == -ENODEV);
    CHECK(mm_seg_find(mm, va)->vmo == memory && ((uint8_t *)pfn_to_virt(page))[0] == 0x69);
    CHECK(!driver_register(driver) && gpu->drv == driver && gpu->drv_priv);
    class_device_t *fresh = class_device_ref_for_device(gpu);
    CHECK(fresh && fresh != old && !gpu_device_access_begin(fresh, &access));
    CHECK(!gpu_device_scanout_get(&access, &scanout) && scanout.memory != memory);
    CHECK(access.ops->flush && !access.ops->flush(access.device, 0, 0, 1, 1));
    CHECK(((uint8_t *)pfn_to_virt(page))[0] == 0x69);
    gpu_scanout_put(&scanout);gpu_device_access_end(&access);
    mm_destroy(mm);class_device_put(fresh);class_device_put(old);device_put(gpu);
}

/* Explicit two-device fixture: logical unregister/re-register through the
 * native core, not a physical PCI hot-unplug or a driver-wide withdrawal. */
static void exercise_gpu_instances(void) {
    device_t *gpus[2] = {0};
    unsigned count = 0;
    for (unsigned index = 0; count < 2; index++) {
        device_t *device = device_find_get_by_class(DEV_CLASS_DISPLAY, index);
        if (!device) break;
        if (device->drv && !strcmp(device->drv->name, "virtio-gpu")) gpus[count++] = device;
        else device_put(device);
    }
    CHECK(count == 2 && gpus[0] != gpus[1]);
    class_device_t *selected = gpu_device_default_get();
    CHECK(selected && (selected->dev == gpus[0] || selected->dev == gpus[1]));
    if (selected->dev == gpus[1]) {
        device_t *swap = gpus[0];gpus[0] = gpus[1];gpus[1] = swap;
    }
    class_device_put(selected);
    class_device_t *old = class_device_ref_for_device(gpus[0]);
    class_device_t *peer = class_device_ref_for_device(gpus[1]);
    CHECK(old && peer && old != peer && gpus[0]->drv_priv != gpus[1]->drv_priv);
    void *peer_private = gpus[1]->drv_priv;
    struct gpu_device_access access;
    struct gpu_scanout first, second;
    CHECK(!gpu_device_access_begin(old, &access));
    CHECK(!gpu_device_scanout_get(&access, &first));
    struct gpu_capabilities capabilities;
    uint32_t negotiated, context_init;
    CHECK(!gpu_device_capabilities_get(&access, &capabilities) &&
          (capabilities.flags & (GPU_CAP_SCANOUT | GPU_CAP_RAM_SCANOUT)) ==
          (GPU_CAP_SCANOUT | GPU_CAP_RAM_SCANOUT));
    CHECK(access.ops->get_features && !access.ops->get_features(access.device, &negotiated, &context_init));
    CHECK(!!(capabilities.flags & GPU_CAP_RENDER) == !!negotiated);
    CHECK(access.ops->flush && !access.ops->flush(access.device, 0, 0, 1, 1));
    gpu_device_access_end(&access);
    CHECK(!gpu_device_access_begin(peer, &access));
    CHECK(!gpu_device_scanout_get(&access, &second));
    CHECK(!gpu_device_capabilities_get(&access, &capabilities) &&
          (capabilities.flags & (GPU_CAP_SCANOUT | GPU_CAP_RAM_SCANOUT)) ==
          (GPU_CAP_SCANOUT | GPU_CAP_RAM_SCANOUT));
    CHECK(access.ops->get_features && !access.ops->get_features(access.device, &negotiated, &context_init));
    CHECK(!!(capabilities.flags & GPU_CAP_RENDER) == !!negotiated);
    CHECK(access.ops->flush && !access.ops->flush(access.device, 0, 0, 1, 1));
    gpu_device_access_end(&access);
    CHECK(first.memory != second.memory);
    mm_struct_t *mm = mm_create();
    CHECK(mm);
    vaddr_t va = mm_mmap_vmo_cap(mm, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, first.memory, first.offset, PROT_READ | PROT_WRITE);
    CHECK(!mm_addr_is_error(va));
    struct vmo *retained = first.memory;
    pfn_t page = vmo_peek_page(retained, first.offset / PAGE_SIZE);
    pfn_t peer_page = vmo_peek_page(second.memory, second.offset / PAGE_SIZE);
    CHECK(page != PFN_NONE && peer_page != PFN_NONE && page != peer_page);
    ((uint8_t *)pfn_to_virt(page))[0] = 0x46;
    ((uint8_t *)pfn_to_virt(peer_page))[0] = 0x79;
    gpu_scanout_put(&first);
    vfile_t *old_file, *peer_file;
    CHECK(!drm_create_vfile_for(old, &old_file) && !drm_create_vfile_for(peer, &peer_file));
    CHECK(!device_unregister(gpus[0]) && !gpus[0]->drv && !gpus[0]->drv_priv);
    CHECK(class_device_binding_stopped(old) && !class_device_binding_stopped(peer));
    CHECK(old_file->ops->ioctl(old_file, DRM_IOCTL_MODE_GETRESOURCES, NULL) == -ENODEV);
    CHECK(!(peer_file->ops->poll(peer_file, POLLIN) & (POLLHUP | POLLERR)));
    CHECK(gpus[1]->drv_priv == peer_private && !gpu_device_access_begin(peer, &access));
    CHECK(!access.ops->flush(access.device, 0, 0, 1, 1));
    gpu_device_access_end(&access);
    selected = gpu_device_default_get();
    CHECK(selected == peer);class_device_put(selected);
    CHECK(!device_register(gpus[0]) && gpus[0]->drv && gpus[0]->drv_priv);
    class_device_t *fresh = class_device_ref_for_device(gpus[0]);
    CHECK(fresh && fresh != old && !gpu_device_access_begin(fresh, &access));
    CHECK(!gpu_device_scanout_get(&access, &first) && first.memory != retained &&
          first.memory != second.memory);
    CHECK(!access.ops->flush(access.device, 0, 0, 1, 1));
    gpu_scanout_put(&first);gpu_device_access_end(&access);
    CHECK(gpus[1]->drv_priv == peer_private && !gpu_device_access_begin(peer, &access));
    CHECK(!access.ops->flush(access.device, 0, 0, 1, 1));
    gpu_device_access_end(&access);
    CHECK(((uint8_t *)pfn_to_virt(page))[0] == 0x46 &&
          ((uint8_t *)pfn_to_virt(peer_page))[0] == 0x79 && mm_seg_find(mm, va)->vmo == retained);
    vfs_put_file(old_file);vfs_put_file(peer_file);
    gpu_scanout_put(&second);mm_destroy(mm);
    class_device_put(fresh);class_device_put(peer);class_device_put(old);
    device_put(gpus[1]);device_put(gpus[0]);
}

static void exercise_physical_mapping(void) {
    /* The mapping borrows this VMO-owned frame. Retain its owner until all
     * physical views are unmapped; the mapper must not pin/put it as RAM. */
    struct vmo *source = vmo_create(VMO_ANONYMOUS, PAGE_SIZE, 0);
    CHECK(source);
    pfn_t page = vmo_get_page(source, 0);
    CHECK(page != PFN_NONE);
    mm_struct_t *mm = mm_create();
    CHECK(mm);
    vaddr_t va = mm_mmap_physical(mm, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, pfn_to_phys(page), PTE_DEVICE);
    CHECK(!mm_addr_is_error(va) && mm_rss_get(mm) == 1 && mm->total_vm == 1);
    CHECK(pt_translate(mm->pgdir, va) == pfn_to_phys(page));
    CHECK(!mm_mprotect(mm, va, PAGE_SIZE, PROT_READ));
    CHECK(!mm_mprotect(mm, va, PAGE_SIZE, PROT_READ | PROT_WRITE));
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
    CHECK(pte && (arch_pte_flags(*pte) & PTE_DEVICE) == PTE_DEVICE);
    CHECK((mm_seg_find(mm, va)->pte_flags & PTE_DEVICE) == PTE_DEVICE);
    CHECK((intptr_t)mm_mmap_physical(mm, va, PAGE_SIZE, PROT_READ,
              MAP_SHARED | MAP_FIXED_NOREPLACE, pfn_to_phys(page), PTE_DEVICE) == -EEXIST);
    CHECK(mm_mmap_physical(mm, va, PAGE_SIZE, PROT_READ,
              MAP_SHARED | MAP_FIXED, pfn_to_phys(page), PTE_DEVICE) == va);
    CHECK(mm_rss_get(mm) == 1 && mm->total_vm == 1);
    CHECK(mm_mprotect(mm, va, PAGE_SIZE, PROT_READ | PROT_WRITE) == -EACCES);
    CHECK(!mm_munmap(mm, va, PAGE_SIZE) && !mm_rss_get(mm) && !mm->total_vm);
    mm_destroy(mm);
    vmo_release(source);
}
#endif

static void exercise_dma_buffer(void) {
    device_t owner = {.name = "dma-buffer-selftest", .references = 1};
    CHECK(!device_register(&owner));
    CHECK(!dma_set_mask(&owner, DMA_MASK_32BIT));
    struct driver_dma_buffer *buffer, *denied = NULL;
    CHECK(!driver_dma_buffer_create(&owner, 33, 65536, &buffer));
    CHECK(owner.references == 3 && driver_dma_buffer_size(buffer) == 33);
    CHECK(!(driver_dma_buffer_address(buffer) & 65535U));
    CHECK(dma_range_ok(&owner, driver_dma_buffer_address(buffer), 33));
    uint8_t *data = driver_dma_buffer_data(buffer);
    CHECK(data);
    for (unsigned i = 0; i < 33; i++) CHECK(!data[i]);
    driver_dma_buffer_get(buffer);
    CHECK(!device_unregister(&owner));
    device_put(&owner);
    CHECK(owner.references == 1);
    CHECK(driver_dma_buffer_create(&owner, 33, 65536, &denied) == -ENODEV && !denied);
    data[32] = 0xa2;
    driver_dma_buffer_put(buffer);
    CHECK(owner.references == 1 && data[32] == 0xa2);
    driver_dma_buffer_put(buffer);
    CHECK(!owner.references);
}

static void exercise_dma_vmo(void) {
    device_t owner = {.name = "dma-vmo-selftest", .references = 1};
    CHECK(!device_register(&owner));
    struct driver_dma_domain *domain;
    struct driver_dma_mapping *mapping;
    CHECK(!driver_dma_domain_create_direct(&owner, &domain)); /* Explicit identity fixture. */
    struct vmo *vmo = vmo_create(VMO_ANONYMOUS, 2 * PAGE_SIZE + 33, 0);
    CHECK(vmo);
    CHECK(driver_dma_map_vmo(domain, vmo, 1, 2, DRIVER_DMA_BIDIRECTIONAL, &mapping) == -EAGAIN && !mapping);
    CHECK(vmo_peek_page(vmo, 1) == PFN_NONE && vmo_peek_page(vmo, 2) == PFN_NONE);
    pfn_t first = vmo_get_page(vmo, 1), second = vmo_get_page(vmo, 2);
    CHECK(first != PFN_NONE && second != PFN_NONE);
    CHECK(!driver_dma_map_vmo(domain, vmo, 1, 2, DRIVER_DMA_BIDIRECTIONAL, &mapping));
    uint64_t address;
    CHECK(driver_dma_mapping_count(mapping) == 2);
    CHECK(!driver_dma_page_address(mapping, 0, &address) && address == pfn_to_phys(first));
    CHECK(!driver_dma_page_address(mapping, 1, &address) && address == pfn_to_phys(second));
    CHECK(!vmo_resize(vmo, 0));
    vmo_release(vmo);
    uint8_t *data = pfn_to_virt(second);
    data[32] = 0xc4;
    CHECK(!device_unregister(&owner));device_put(&owner);
    CHECK(owner.references == 1 && !driver_dma_mapping_sync_for_device(mapping));
    CHECK(!driver_dma_mapping_sync_for_cpu(mapping) && data[32] == 0xc4);
    CHECK(!driver_dma_try_unmap_pages(&mapping) && !mapping);
    driver_dma_domain_put(domain);CHECK(!owner.references);
}

static void exercise_dma_buffer_mapping(void) {
    device_t owner = {.name = "dma-buffer-map-selftest", .references = 1};
    CHECK(!device_register(&owner));
    struct driver_dma_buffer *buffer;
    struct driver_dma_domain *domain;
    struct driver_dma_mapping *mapping;
    CHECK(!driver_dma_buffer_create(&owner, PAGE_SIZE + 33, 65536, &buffer));
    CHECK(!driver_dma_domain_create_direct(&owner, &domain)); /* Explicit identity fixture. */
    CHECK(!driver_dma_map_buffer(domain, buffer, DRIVER_DMA_BIDIRECTIONAL, &mapping));
    CHECK(driver_dma_mapping_count(mapping) == 2);
    uint64_t physical = driver_dma_buffer_address(buffer), address;
    CHECK(!driver_dma_page_address(mapping, 0, &address) && address == physical);
    CHECK(!driver_dma_page_address(mapping, 1, &address) && address == physical + PAGE_SIZE);
    uint8_t *data = driver_dma_buffer_data(buffer);
    data[PAGE_SIZE + 32] = 0xb3;
    driver_dma_buffer_put(buffer);
    CHECK(!device_unregister(&owner));
    device_put(&owner);
    CHECK(owner.references == 2 && data[PAGE_SIZE + 32] == 0xb3);
    CHECK(!driver_dma_mapping_sync_for_device(mapping));
    CHECK(!driver_dma_mapping_sync_for_cpu(mapping));
    CHECK(!driver_dma_try_unmap_pages(&mapping) && !mapping && owner.references == 1);
    driver_dma_domain_put(domain);
    CHECK(!owner.references);
}

/* Explicit scratch-disk fixture only: QEMU xHCI + blank 8 MiB USB disk. */
static void exercise_usb_bulk(void) {
    pci_user_device_info_t info;
    CHECK(!pci_user_device_find(0x1b36, 0x000d, &info));
    device_t *controller = NULL, *disk = NULL;
    CHECK(!pci_device_find_get(0, info.devid, &controller));
    usb_core_scan();
    for (int index = 0;; index++) {
        device_t *candidate = device_find_get_by_class(DEV_CLASS_BLOCK, index);
        if (!candidate) break;
        if (candidate->parent == controller && candidate->drv &&
            !strcmp(candidate->drv->name, "usb-storage")) { disk = candidate; break; }
        device_put(candidate);
    }
    CHECK(disk && disk->plat_data);
    usb_device_t *protocol = ((usb_interface_t *)disk->plat_data)->dev;
    CHECK(protocol && protocol->vendor == 0x46f4 && protocol->product == 1);
    class_device_t *class = class_device_ref_for_device(disk);
    const void *operations = NULL;
    CHECK(class && !class_device_ops_begin(class, DEV_CLASS_BLOCK, &operations));
    const block_dev_ops_t *ops = operations;
    CHECK(ops->read && ops->write && ops->capacity && ops->sector_size);
    CHECK(ops->sector_size(disk) == 512 && ops->capacity(disk) == 16384);
    uint8_t *bytes = kmalloc(8192);
    CHECK(bytes && !ops->read(disk, 0, bytes, 8));
    for (unsigned i = 0; i < 4096; i++) CHECK(!bytes[i]);
    CHECK(!ops->read(disk, 8, bytes + 4096, 8));
    for (unsigned i = 0; i < 4096; i++) CHECK(!bytes[i + 4096]);
    for (unsigned i = 0; i < 4096; i++) bytes[i] = (uint8_t)(i * 37U + 11U);
    CHECK(!ops->write(disk, 8, bytes, 8));
    memset(bytes + 4096, 0, 4096);
    CHECK(!ops->read(disk, 8, bytes + 4096, 8));
    CHECK(!memcmp(bytes, bytes + 4096, 4096));
    memset(bytes, 0, 4096);
    CHECK(!ops->write(disk, 8, bytes, 8));
    CHECK(!ops->read(disk, 8, bytes + 4096, 8));
    CHECK(!memcmp(bytes, bytes + 4096, 4096));
    kfree(bytes);
    class_device_call_end(class);
    class_device_put(class);
    device_put(disk);
    device_put(controller);
    kinfo("DRIVER_USB_BULK: PASS read=1 write=1 verify=1 restore=1\n");
}

static void exercise_xhci_remove(void) {
    pci_user_device_info_t info;
    CHECK(!pci_user_device_find(0x1b36, 0x000d, &info));
    device_t *controller = NULL;
    CHECK(!pci_device_find_get(0, info.devid, &controller));
    CHECK(controller->drv && !strcmp(controller->drv->name, "xhci-hcd"));
    usb_core_scan();
    exercise_usb_interfaces(controller);
    device_t *old = xhci_input_child(controller);
    CHECK(old);
    class_device_t *old_class = class_device_ref_for_device(old);
    CHECK(old_class);
    driver_t *driver = controller->drv; /* Embedded fixture owns static code. */
    CHECK(!driver_unregister(driver));
    CHECK(device_is_registered(controller) && !controller->drv && !controller->drv_priv);
    uint16_t command = pci_cfg_read16(controller, 4);
    CHECK(command != 0xffffU && !(command & 4U));
    CHECK(!device_is_registered(old) && !old->drv && !old->plat_data);
    CHECK(class_device_call_begin(old_class) == -ENODEV);
    CHECK(!driver_register(driver) && controller->drv == driver);
    usb_core_scan();
    device_t *fresh = xhci_input_child(controller);
    CHECK(fresh && fresh != old && fresh->drv);
    device_put(fresh);
    class_device_put(old_class);
    device_put(old);
    device_put(controller);
    kinfo("DRIVER_XHCI_REMOVE: PASS prepare=1 stopped=1 children=1 rebind=1\n");
}

static void exercise_driver_protocols(void)
{
    class_device_t *network = class_device_get_by_type(DEV_CLASS_NET, 0);
    CHECK(network && driver_endpoint_protocol_count(network) >= 2);
    bool primary = false, extension = false;
    for (size_t i = 0; i < driver_endpoint_protocol_count(network); i++) {
        struct driver_protocol_info info;
        CHECK(!driver_endpoint_protocol_info(network, i, &info));
        primary |= info.id == DRIVER_PROTOCOL_NET;
        extension |= info.id == DRIVER_PROTOCOL_NET_STATS;
    }
    CHECK(primary && extension);
    net_dev_stats_t before, after;
    CHECK(!class_net_read_stats(network, &before));
    const struct driver_protocol_requirement need = {
        DRIVER_PROTOCOL_NET, 1,
        __builtin_offsetof(net_dev_ops_t, send) + sizeof(((net_dev_ops_t *)0)->send), 0,
    };
    struct driver_protocol_access access;
    CHECK(!driver_protocol_begin(network, &need, &access));
    const net_dev_ops_t *ops = access.ops;
    /* A minimum Ethernet frame exercises the actual provider, not a test ops
     * table. Submission counters are not a remote-delivery/DMA-stop proof. */
    uint8_t frame[60] = {0xff,0xff,0xff,0xff,0xff,0xff,2,0,0,0,0,1,0x88,0xb5};
    CHECK(ops->send && ops->send(access.device, frame, sizeof(frame)) == sizeof(frame));
    driver_protocol_end(&access);
    class_net_poll(network);
    CHECK(!class_net_read_stats(network, &after) && after.tx_packets > before.tx_packets);
    class_device_put(network);
    kinfo("DRIVER_PROTOCOLS: PASS primary=1 extension=1 send=1 stats=1\n");
}

void driver_runtime_selftest_runtime(void)
{
    if (!boot_flag(bootargs_get(), "driver.protocol_test=1")) return;
    exercise_driver_protocols();
    if (current_board && current_board->poweroff) current_board->poweroff();
    panic("DRIVER_PROTOCOLS: poweroff returned");
}

void driver_runtime_selftest_boot(void)
{
    const char *args = bootargs_get();
    if (!boot_flag(args, "driver.runtime_test=1"))
        return;
    kinfo("DRIVER_RUNTIME: START\n");
    exercise_resources();
    exercise_gpu_access();
    bool gpu_caps_test = boot_flag(args, "driver.gpu_caps_test=1");
    if (gpu_caps_test) exercise_gpu_capabilities();
#ifndef CONFIG_NOMMU
    exercise_physical_mapping();
    exercise_gpu_scanout();
    bool gpu_stop = boot_flag(args, "driver.gpu_stop_test=1");
    if (gpu_stop) exercise_gpu_stop();
    bool gpu_instances = boot_flag(args, "driver.gpu_instances_test=1");
    if (gpu_instances) exercise_gpu_instances();
    bool drm_binding = boot_flag(args, "driver.drm_binding_test=1");
    if (drm_binding) {
        class_device_t *publication = gpu_device_default_get();
        CHECK(publication && !drm_native_binding_selftest(publication));
        class_device_put(publication);
    }
#endif
    exercise_dma_buffer();
    exercise_dma_buffer_mapping();
    exercise_dma_vmo();
    exercise();
    exercise_fence_notifications();
    exercise_requests();
    exercise_firmware();
    if (boot_flag(args, "driver.usb_bulk_test=1"))
        exercise_usb_bulk();
    if (boot_flag(args, "driver.xhci_remove_test=1"))
        exercise_xhci_remove();
    if (boot_flag(args, "driver.iommu_native_test=1"))
        driver_iommu_native_selftest();
    bool amdvi_owner = boot_flag(args, "driver.amdvi_owner_test=1");
    if (amdvi_owner)
        CHECK(!amd_iommu_owner_selftest());
    bool pci_scope = boot_flag(args, "driver.pci_scope_test=1");
    if (pci_scope)
        exercise_pci_scope();
    bool requester = boot_flag(args, "driver.amdvi_requester_test=1");
    bool policy_test = boot_flag(args, "driver.amdvi_policy_test=1");
    if (requester || policy_test || boot_flag(args, "driver.amdvi_route_test=1"))
        exercise_amdvi_route(requester, policy_test);
    /* Keep all witnesses in one end-of-test console record. A best-effort
     * SMP console must not show overall PASS while dropping only the resource
     * result printed during concurrent background-thread startup. */
    kinfo("%s"
          "%s"
#ifndef CONFIG_NOMMU
          "%s"
          "%s"
          "%s"
          "DRIVER_GPU_SCANOUT: PASS vmo=1 unbind=1 split=1 fork=1 pins=1\n"
          "DRIVER_PHYSICAL_MAP: PASS placement=1 protect=1 accounting=1\n"
#endif
          "DRIVER_GPU_ACCESS: PASS publication=1 offline=1 refs=1 caps=1\n"
          "DRIVER_DMA_VMO: PASS snapshot=1 resize=1 lifetime=1\n"
          "DRIVER_DMA_BUFFER_MAP: PASS pages=2 retained=1 unmap=1\n"
          "DRIVER_DMA_BUFFER: PASS align=1 owner_refs=1 lifetime=1\n"
          "DRIVER_RESOURCES: PASS claims=1 conflicts=1 owner_refs=1 devres=1 inventory=1 memory=1%s\n"
          "DRIVER_QUEUE: PASS parallel=1 worker=1 completion=1 stop_retry=1 drain=1\n"
          "DRIVER_FENCE_WATCH: PASS notify=1 cancel=1 error=1 work=1\n"
          "DRIVER_RUNTIME: PASS delayed=1 retime=1 cancel=1 requeue=1 destroy_timeout=1 park_wake=1 firmware=1\n",
          amdvi_owner ? "AMDVI_OWNER: PASS claim=1 rebind=1 writes=0\n" : "",
          gpu_caps_test ? "DRIVER_GPU_CAPS: PASS native=1 firmware=1 headless=1 election=1\n" : "",
#ifndef CONFIG_NOMMU
          gpu_stop ? "DRIVER_GPU_STOP: PASS reset=1 unbind=1 retained=1 rebind=1\n" : "",
          drm_binding ? "DRIVER_DRM_BINDING: PASS file=1 stop=1 reaper=1 vma=1 rebind=1\n" : "",
          gpu_instances ? "DRIVER_GPU_INSTANCES: PASS devices=2 independent=1 stop=1 retained=1 rebind=1 caps=1\n" : "",
#endif
          pci_scope ? " pci=1" : "");
    if (current_board && current_board->poweroff)
        current_board->poweroff();
    panic("DRIVER_RUNTIME: poweroff returned");
}
#endif
