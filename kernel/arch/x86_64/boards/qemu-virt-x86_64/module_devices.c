#include "../../../common/module_devices.h"
#include "core/klog.h"
#include "drivers/bus/platform_bus.h"

static const char ps2_id[] = "PNP0303";
static const fw_static_property_t properties[] = {
    {"hardware-id", {ps2_id, sizeof(ps2_id), FW_ORDER_NATIVE}},
};
static const fw_resource_t resources[] = {
    {FW_RESOURCE_IO, 0x60, 1, 0, "data"},
    {FW_RESOURCE_IO, 0x64, 1, 0, "status-command"},
};
static const fw_static_node_t nodes[] = {
    {.name="board-fallback", .parent=SIZE_MAX},
    {.name="ps2", .parent=0, .properties=properties, .property_count=1,
     .resources=resources, .resource_count=2},
};
static const fw_static_tree_t tree = {nodes, sizeof(nodes)/sizeof(nodes[0])};
void board_configure_devices(void) {
    device_t *ps2 = device_find_get_by_firmware_id(ps2_id);
    int firmware = ps2 != NULL;
    int on_platform = ps2 && ps2->bus == &platform_bus;
    if (ps2)
        device_put(ps2);
    kinfo("[PS2] board: firmware=%d platform=%d; %s\n",
          firmware, on_platform,
          on_platform ? "keeping firmware device" : "creating static i8042");
    if (!on_platform)
        board_firmware_static_enumerate(&tree);
    /* TPMs are described by their ACPI device resources. A TPM2 table address
     * alone does not identify TIS versus CRB or a usable register span. */
}
