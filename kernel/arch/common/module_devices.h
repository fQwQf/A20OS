#ifndef A20_BOARD_MODULE_DEVICES_H
#define A20_BOARD_MODULE_DEVICES_H
#include "firmware/static.h"
#include "firmware/device.h"
#include "drivers/core/driver_core.h"
#include "core/panic.h"

/* Deployment policy acts on the canonical firmware device. A compile-time
 * fallback is admitted only when firmware did not describe that hardware. */
static inline int board_firmware_device_exists(uint64_t physical,
                                               const char *id) {
    device_t *device = physical ? device_find_get_by_mmio(physical) : NULL;
    if (!device && id)
        device = device_find_get_by_firmware_id(id);
    if (!device)
        return 0;
    device_put(device);
    return 1;
}
static inline void board_firmware_user_owned(uint64_t physical, const char *id,
                                             int owned) {
    device_t *device = physical ? device_find_get_by_mmio(physical) : NULL;
    if (!device && id)
        device = device_find_get_by_firmware_id(id);
    if (device) {
        device->user_owned = owned;
        device_put(device);
    }
}
static inline void
board_firmware_static_enumerate(const fw_static_tree_t *tree) {
    fw_node_t root;
    if (fw_static_init(tree, &root) || firmware_device_enumerate(&root) < 0)
        panic("board: invalid static device description");
}
#endif
