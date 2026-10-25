#include "firmware/device.h"
#include "firmware/irq.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/sync.h"
#include "mm/mm.h"

typedef struct firmware_device {
    platform_device_t platform;
    size_t memory_count, irq_count;
    int resource_error;
    struct firmware_device *next;
    struct firmware_device *transaction_next;
} firmware_device_t;
static firmware_device_t *devices;
static mutex_t devices_lock = MUTEX_INIT;
static void unlink_device(firmware_device_t *owned) {
    mutex_lock(&devices_lock);
    for (firmware_device_t **p = &devices; *p; p = &(*p)->next)
        if (*p == owned) {
            *p = owned->next;
            break;
        }
    mutex_unlock(&devices_lock);
}
static void firmware_device_release(device_t *dev) {
    firmware_device_t *owned = (firmware_device_t *)dev;
    unlink_device(owned);
    kfree(dev->res);
    kfree(owned);
}

struct device *firmware_device_find(const fw_node_t *node) {
    return device_find_get_by_firmware(node);
}
int device_dependency_get(struct device *consumer, const char *property,
                          const char *cells, size_t index,
                          struct device **provider, fw_reference_t *reference) {
    if (!consumer || !provider || !reference)
        return -EINVAL;
    *provider = NULL;
    int rc = fw_node_reference(&consumer->firmware_node, property, cells, index,
                               reference);
    if (rc)
        return rc;
    device_t *found = firmware_device_find(&reference->node);
    if (!found)
        return -EPROBE_DEFER;
    rc = device_add_dependency(consumer, found);
    if (!rc && (!found->drv || (found->state != DEV_STATE_PROBED &&
                                found->state != DEV_STATE_RUNNING)))
        rc = -EPROBE_DEFER;
    if (rc) {
        device_put(found);
        return rc;
    }
    *provider = found;
    return 0;
}
int firmware_device_prepare(struct device *device) {
    firmware_device_t *owned;
    mutex_lock(&devices_lock);
    for (owned = devices; owned; owned = owned->next)
        if (&owned->platform.dev == device)
            break;
    mutex_unlock(&devices_lock);
    if (!owned)
        return 0;
    if (owned->resource_error)
        return owned->resource_error;
    for (size_t i = 0; i < owned->irq_count; i++) {
        fw_reference_t reference;
        uint32_t irq, flags;
        resource_t *resource = &device->res[owned->memory_count + i];
        *resource = (resource_t){0};
        int rc = fw_node_irq(&device->firmware_node, i, &reference);
        if (rc)
            return rc;
        rc = fw_irq_resolve(&reference, &irq, &flags);
        if (rc == -EPROBE_DEFER && device->drv && device->drv->irq_optional)
            continue;
        if (rc)
            return rc;
        *resource = (resource_t){
            .type = RES_IRQ, .start = irq, .end = irq, .flags = flags};
    }
    return 0;
}
static int has_identity(const fw_node_t *node) {
    fw_property_t property;
    const char *names[] = {"compatible", "hardware-id", "acpi,hid", "acpi,cid"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int rc = fw_node_property(node, names[i], &property);
        if (!rc && property.size)
            return 1;
        if (rc && rc != -ENOENT)
            return rc;
    }
    return 0;
}
static int register_node(const fw_node_t *node, device_t *parent,
                         firmware_device_t **created) {
    device_t *existing = firmware_device_find(node);
    if (existing) {
        device_put(existing);
        return 0;
    }
    int rc = has_identity(node);
    if (rc <= 0)
        return rc;
    const char *type;
    if (!fw_node_read_string(node, "device_type", 0, &type) &&
        (!strcmp(type, "cpu") || !strcmp(type, "memory")))
        return 0;
    size_t memory_count = 0, irq_count = 0;
    int resource_error = 0;
    fw_resource_t range;
    fw_reference_t reference;
    while (!(rc = fw_node_resource(node, memory_count, &range)))
        memory_count++;
    if (rc == -EOPNOTSUPP) {
        resource_error = rc;
        memory_count = 0;
    } else if (rc != -ENOENT)
        return rc;
    while (!(rc = fw_node_irq(node, irq_count, &reference)))
        irq_count++;
    if (rc == -EOPNOTSUPP) {
        resource_error = rc;
        irq_count = 0;
    } else if (rc != -ENOENT)
        return rc;
    if (memory_count > 0x7fffffffU || irq_count > 0x7fffffffU - memory_count ||
        memory_count + irq_count > SIZE_MAX / sizeof(resource_t))
        return -EOVERFLOW;
    firmware_device_t *owned = kcalloc(1, sizeof(*owned));
    if (!owned)
        return -ENOMEM;
    owned->memory_count = memory_count;
    owned->irq_count = irq_count;
    owned->resource_error = resource_error;
    device_t *dev = &owned->platform.dev;
    dev->name = fw_node_name(node);
    if (!dev->name) {
        kfree(owned);
        return -EINVAL;
    }
    dev->firmware_node = *node;
    dev->release = firmware_device_release;
    dev->parent = parent;
    dev->res_count = (int)(memory_count + irq_count);
    if (dev->res_count) {
        dev->res = kcalloc((size_t)dev->res_count, sizeof(resource_t));
        if (!dev->res) {
            kfree(owned);
            return -ENOMEM;
        }
    }
    for (size_t i = 0; i < memory_count; i++) {
        rc = fw_node_resource(node, i, &range);
        if (rc)
            goto failed;
        dev->res[i] = (resource_t){
            .type = range.type == FW_RESOURCE_IO ? RES_IOPORT : RES_MMIO,
            .start = range.address,
            .end = range.address + range.size - 1,
            .flags = range.flags,
            .name = range.name};
    }
    /* Publish ownership before registration, since registration can probe. */
    mutex_lock(&devices_lock);
    owned->next = devices;
    devices = owned;
    mutex_unlock(&devices_lock);
    rc = platform_device_register(&owned->platform);
    if (!rc) {
        owned->transaction_next = *created;
        *created = owned;
        device_put(dev); /* Registry owns the published object. */
        return 0;
    }
    unlink_device(owned);
failed:
    kfree(dev->res);
    kfree(owned);
    return rc;
}
static int walk(const fw_node_t *node, device_t *parent, unsigned depth,
                firmware_device_t **created) {
    if (depth > 64)
        return -ELOOP;
    int rc = fw_node_available(node);
    if (rc < 0)
        return rc;
    if (rc && (rc = register_node(node, parent, created)) < 0)
        return rc;
    rc = fw_node_children_available(node);
    if (rc <= 0)
        return rc;
    device_t *registered = firmware_device_find(node);
    if (registered)
        parent = registered;
    fw_node_t child, previous;
    int has_previous = 0;
    while ((rc = fw_node_next_child(node, has_previous ? &previous : NULL,
                                    &child)) == 0) {
        previous = child;
        has_previous = 1;
        rc = walk(&child, parent, depth + 1, created);
        if (rc)
            break;
    }
    if (registered)
        device_put(registered);
    return rc == -ENOENT ? 0 : rc;
}
int firmware_device_enumerate(const fw_node_t *root) {
    if (!fw_node_valid(root))
        return -EINVAL;
    driver_discovery_begin();
    fw_irq_domain_t *created;
    int rc = firmware_irq_domains_prepare(root, &created);
    if (rc) {
        driver_discovery_end();
        return rc;
    }
    int before = driver_device_count();
    firmware_device_t *discovered = NULL;
    rc = walk(root, NULL, 0, &discovered);
    if (rc) {
        /* Children were prepended after parents. Roll back in reverse order
         * while discovery still prevents probes and their side effects. */
        firmware_device_t *owned = discovered;
        while (owned) {
            firmware_device_t *next = owned->transaction_next;
            device_unregister(&owned->platform.dev);
            owned = next;
        }
        firmware_irq_domains_abort(created);
    }
    driver_discovery_end();
    if (rc) {
        kerr("[FIRMWARE] %s enumeration failed at %d registered devices: %d\n",
             fw_node_source(root), driver_device_count() - before, rc);
        return rc;
    }
    int count = driver_device_count() - before;
    kinfo("[FIRMWARE] %s: %d devices discovered\n", fw_node_source(root),
          count);
    return count;
}
