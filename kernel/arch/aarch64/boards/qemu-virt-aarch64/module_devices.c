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
    /* Only claim the RTC that firmware actually describes on this board. */
    board_firmware_user_owned(0x09010000, NULL, 1);
}

/* QEMU virt's PL031 page. Never authorize the Goldfish address or RAM. */
#define USER_RTC_BASE 0x09010000ULL
#define USER_RTC_SIZE 0x1000ULL
int board_user_mmio_present(uint64_t physical) {
    return physical >= USER_RTC_BASE &&
           physical - USER_RTC_BASE < USER_RTC_SIZE &&
           board_firmware_device_exists(USER_RTC_BASE, NULL);
}
int board_user_mmio_allowed(uint64_t physical, uint64_t size) {
    return size && physical >= USER_RTC_BASE &&
           physical - USER_RTC_BASE < USER_RTC_SIZE &&
           size <= USER_RTC_SIZE - (physical - USER_RTC_BASE) &&
           board_user_mmio_present(physical);
}
int board_user_mmio_owned(uint64_t physical) {
    return board_user_mmio_present(physical);
}
int board_user_mmio_index(uint64_t physical) {
    return board_user_mmio_present(physical) ? 0 : -1;
}
