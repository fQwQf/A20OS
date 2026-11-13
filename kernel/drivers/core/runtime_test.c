/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/runtime_test.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/fence.h"
#include "drivers/core/workqueue.h"
#include "drivers/core/firmware.h"
#include "drivers/core/amd_iommu.h"
#include "drivers/core/dma_mapping.h"
#include "drivers/core/memory.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/usb/usb.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "core/bootargs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/fcntl.h"
#include "fs/vfs.h"
#include "fs/file.h"

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

void driver_runtime_selftest_boot(void)
{
    const char *args = bootargs_get();
    if (!boot_flag(args, "driver.runtime_test=1"))
        return;
    kinfo("DRIVER_RUNTIME: START\n");
    exercise_resources();
    exercise_dma_buffer();
    exercise();
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
    kinfo("%sDRIVER_DMA_BUFFER: PASS align=1 owner_refs=1 lifetime=1\n"
          "DRIVER_RESOURCES: PASS claims=1 conflicts=1 owner_refs=1 devres=1 inventory=1 memory=1%s\n"
          "DRIVER_RUNTIME: PASS delayed=1 retime=1 cancel=1 requeue=1 destroy_timeout=1 park_wake=1 firmware=1\n",
          amdvi_owner ? "AMDVI_OWNER: PASS claim=1 rebind=1 writes=0\n" : "",
          pci_scope ? " pci=1" : "");
    if (current_board && current_board->poweroff)
        current_board->poweroff();
    panic("DRIVER_RUNTIME: poweroff returned");
}
#endif
