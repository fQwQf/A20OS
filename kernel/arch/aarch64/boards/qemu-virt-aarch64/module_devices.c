#include "../../../common/module_devices.h"
static const char virtio_id[] = "virtio,mmio";
static const fw_static_property_t properties[] = {
    {"compatible", {virtio_id, sizeof(virtio_id), FW_ORDER_NATIVE}},
};
static const fw_resource_t resources[] = {
    {FW_RESOURCE_MEMORY, 0x0a000a00, 0x200, IORESOURCE_MMIO_32BIT, "vinput"},
};
static const fw_static_node_t nodes[] = {
    {.name="board-fallback", .parent=SIZE_MAX},
    {.name="virtio-input-slot5", .parent=0, .properties=properties, .property_count=1,
     .resources=resources, .resource_count=1},
};
static const fw_static_tree_t tree = {nodes, sizeof(nodes)/sizeof(nodes[0])};
void board_configure_devices(void) {
    if (!board_firmware_device_exists(0x0a000a00, NULL))
        board_firmware_static_enumerate(&tree);
    board_firmware_user_owned(0x0a000a00, NULL, 1);
    /* This machine describes PL031 via DT; no Goldfish RTC exists at 0x101000. */
}
