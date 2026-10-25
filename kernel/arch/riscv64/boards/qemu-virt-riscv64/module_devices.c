#include "../../../common/module_devices.h"
#include "drivers/core/udriver.h"

static const char rtc_id[] = "google,goldfish-rtc";
static const char virtio_id[] = "virtio,mmio";
static const fw_static_property_t rtc_properties[] = {
    {"compatible", {rtc_id, sizeof(rtc_id), FW_ORDER_NATIVE}},
};
static const fw_static_property_t virtio_properties[] = {
    {"compatible", {virtio_id, sizeof(virtio_id), FW_ORDER_NATIVE}},
};
static const fw_resource_t rtc_resources[] = {
    {FW_RESOURCE_MEMORY, 0x101000, 0x1000, IORESOURCE_MMIO_32BIT, "rtc"},
};
static const fw_resource_t block_resources[] = {
    {FW_RESOURCE_MEMORY, 0x10004000, 0x1000, IORESOURCE_MMIO_32BIT, "ubd"},
};
static const fw_resource_t input_resources[] = {
    {FW_RESOURCE_MEMORY, 0x10006000, 0x1000, IORESOURCE_MMIO_32BIT, "vinput"},
};
static const fw_static_node_t rtc_nodes[] = {
    {.name="board-fallback", .parent=SIZE_MAX},
    {.name="goldfish-rtc", .parent=0, .properties=rtc_properties, .property_count=1,
     .resources=rtc_resources, .resource_count=1},
};
static const fw_static_node_t block_nodes[] = {
    {.name="board-fallback", .parent=SIZE_MAX},
    {.name="virtio-blk-user", .parent=0, .properties=virtio_properties, .property_count=1,
     .resources=block_resources, .resource_count=1},
};
static const fw_static_node_t input_nodes[] = {
    {.name="board-fallback", .parent=SIZE_MAX},
    {.name="virtio-input-slot5", .parent=0, .properties=virtio_properties, .property_count=1,
     .resources=input_resources, .resource_count=1},
};
static const fw_static_tree_t rtc_tree = {rtc_nodes, 2};
static const fw_static_tree_t block_tree = {block_nodes, 2};
static const fw_static_tree_t input_tree = {input_nodes, 2};
void board_configure_devices(void) {
    if (!board_firmware_device_exists(0x101000, rtc_id))
        board_firmware_static_enumerate(&rtc_tree);
    if (!board_firmware_device_exists(0x10004000, NULL))
        board_firmware_static_enumerate(&block_tree);
    if (!board_firmware_device_exists(0x10006000, NULL))
        board_firmware_static_enumerate(&input_tree);
    /* Preserve the board's GUI exception for slot 5, and slot 3 ownership. */
    board_firmware_user_owned(0x101000, rtc_id, udriver_mmio_user_owned(0x101000));
    board_firmware_user_owned(0x10004000, NULL, udriver_mmio_user_owned(0x10004000));
    board_firmware_user_owned(0x10006000, NULL, udriver_mmio_user_owned(0x10006000));
}
