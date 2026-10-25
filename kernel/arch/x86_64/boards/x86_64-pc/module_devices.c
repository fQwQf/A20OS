#include "../../../common/module_devices.h"

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
    if (!board_firmware_device_exists(0, ps2_id))
        board_firmware_static_enumerate(&tree);
    /* TPMs are described by their ACPI device resources. A TPM2 table address
     * alone does not identify TIS versus CRB or a usable register span. */
}
