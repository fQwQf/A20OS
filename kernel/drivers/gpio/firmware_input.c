/* Read-only adaptation of firmware GPIO references for polled consumers.
 * AMDI0030 uses a 32-bit per-pin register, PinStatus bit 16. Add a new
 * controller layout here rather than in HID or another consumer protocol. */
#include "drivers/gpio/firmware_input.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/string.h"

int gpio_firmware_input_get(const fw_node_t *consumer, const char *property,
                            gpio_firmware_input_t *out)
{
    if (!consumer || !property || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    fw_reference_t ref;
    int ret = fw_node_reference(consumer, property, "#gpio-cells", 0, &ref);
    if (ret)
        return ret;
    if (ref.count != 2 || ref.args[1] > 1)
        return -EINVAL;
    if (fw_node_match_id(&ref.node, "AMDI0030") != 1)
        return -EOPNOTSUPP;
    uint64_t offset = (uint64_t)ref.args[0] * sizeof(uint32_t);
    fw_resource_t resource;
    for (size_t n = 0; fw_node_resource(&ref.node, n, &resource) == 0; n++) {
        if (resource.type != FW_RESOURCE_MEMORY)
            continue;
        if (resource.size < sizeof(uint32_t) ||
            offset > resource.size - sizeof(uint32_t) ||
            resource.address > (~(uint64_t)0) - offset - (sizeof(uint32_t) - 1))
            return -ERANGE;
        out->reg = arch_device_map_mmio(resource.address + offset,
                                        sizeof(uint32_t), 0);
        if (!out->reg)
            return -ENOMEM;
        out->mask = 1U << 16;
        out->active_low = ref.args[1];
        return 0;
    }
    return -ENODEV;
}

int gpio_firmware_input_pending(const gpio_firmware_input_t *input)
{
    /* Absence of an optional attention line means poll the device itself. */
    return !input || !input->reg ||
           (!!(readl(input->reg) & input->mask) != input->active_low);
}
