#include "drivers/bus/platform_bus.h"
#include "core/errno.h"

static int platform_match(device_t *dev, const driver_t *drv)
{
    if (!dev || !drv || drv->bus != &platform_bus || !dev->hardware_id ||
        !drv->id_table)
        return 0;
    for (const device_id_t *id = drv->id_table;
         id->vendor || id->device; id++) {
        if ((id->vendor == VENDOR_ANY ||
             id->vendor == dev->hardware_id->vendor) &&
            (id->device == DEVICE_ANY ||
             id->device == dev->hardware_id->device)) {
            dev->matched_id = id;
            return 1;
        }
    }
    return 0;
}

bus_type_t platform_bus = {
    .name = "platform",
    .match = platform_match,
};

int platform_device_register(platform_device_t *pdev)
{
    if (!pdev || !pdev->dev.name)
        return -EINVAL;
    int ret = bus_register(&platform_bus);
    if (ret < 0 && ret != -EEXIST)
        return ret;
    pdev->dev.bus = &platform_bus;
    pdev->dev.hardware_id = &pdev->id;
    return device_register(&pdev->dev);
}

void platform_device_unregister(platform_device_t *pdev)
{
    if (pdev)
        device_unregister(&pdev->dev);
}

/*
 * See the PLATFORM_IRQ_RESOURCE_CHANNEL contract in
 * kernel/include/drivers/bus/platform_bus.h for the return-value contract.
 *
 * This reads the same resource_t the board or the device tree enumerator
 * wrote; it does not add a second IRQ description channel, so a device that
 * already publishes RES_IRQ (the StarFive and LS2K GMACs do, and the RISC-V
 * device tree walker fills it from `interrupts`) is picked up here with no
 * board change at all.
 */
int platform_device_irq(device_t *dev)
{
    if (!dev || dev->bus != &platform_bus)
        return -EINVAL;

    /* Index 0: the platform bus publishes one line per device.  A second
     * RES_IRQ is not a thing this bus has ever described, and silently using
     * the first one of several would attach a driver to the wrong line. */
    resource_t *res = device_get_resource(dev, RES_IRQ, 0);
    if (!res)
        return -ENODEV;              /* no line published: caller polls */

    /* Validate before subtracting, the same order core-model.md prescribes for
     * resource lengths: a malformed resource is a board bug, not a line. */
    if (res->end < res->start || res->end != res->start)
        return -EINVAL;
    if (res->start >= PLATFORM_IRQ_MAX_LINES)
        return -ERANGE;

    return (int)res->start;
}
