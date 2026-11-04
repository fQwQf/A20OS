#include "drivers/bus/platform_bus.h"
#include "core/errno.h"
#include "core/string.h"

static int platform_match(device_t *dev, const driver_t *drv)
{
    if (!dev || !drv || drv->bus != &platform_bus) return 0;
    dev->firmware_match = NULL;
    int has_node = fw_node_valid(&dev->firmware_node);
    const char *source = has_node ? fw_node_source(&dev->firmware_node) : NULL;
    for (const firmware_device_id_t *id = drv->firmware_ids; id && id->id; id++) {
        if (id->source && (!source || strcmp(id->source, source)))
            continue;
        /* A static board device has no firmware node; software_id is the
         * identity the board supplied for exactly that case. */
        if ((has_node && fw_node_match_id(&dev->firmware_node, id->id) > 0) ||
            (dev->software_id && !strcmp(dev->software_id, id->id))) {
            dev->firmware_match = id;
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
    return device_register(&pdev->dev);
}

void platform_device_unregister(platform_device_t *pdev)
{
    if (pdev)
        device_unregister(&pdev->dev);
}

/* Resources already contain domain-resolved driver IRQ identifiers. */
int platform_device_irq(device_t *dev)
{
    if (!dev || dev->bus != &platform_bus)
        return -EINVAL;

    /* Drivers needing additional lines use device_get_resource() by index. */
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
