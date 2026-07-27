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
