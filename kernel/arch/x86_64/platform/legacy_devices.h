#ifndef A20_X86_LEGACY_DEVICES_H
#define A20_X86_LEGACY_DEVICES_H
#include "../../common/module_devices.h"
/* PC-AT fixed I/O fallback. ACPI HIDs are canonical when present. */
static inline void x86_legacy_configure_devices(void) {
    static const char speaker_id[] = "PNP0800", rtc_id[] = "PNP0B00";
    static const fw_static_property_t speaker_properties[] =
        {
            {"hardware-id", {speaker_id, sizeof(speaker_id), FW_ORDER_NATIVE}},
        },
                                      rtc_properties[] = {
                                          {"hardware-id",
                                           {rtc_id, sizeof(rtc_id),
                                            FW_ORDER_NATIVE}},
                                      };
    static const fw_resource_t speaker_resources[] =
        {
            {FW_RESOURCE_IO, 0x42, 2, 0, "pit-channel2"},
            {FW_RESOURCE_IO, 0x61, 1, 0, "speaker-control"},
        },
                               rtc_resources[] = {
                                   {FW_RESOURCE_IO, 0x70, 1, 0, "cmos-index"},
                                   {FW_RESOURCE_IO, 0x71, 1, 0, "cmos-data"},
                               };
    static fw_static_node_t nodes[] = {
        {.name = "pc-legacy-fallback", .parent = SIZE_MAX},
        {.name = "pc-speaker",
         .parent = 0,
         .properties = speaker_properties,
         .property_count = 1,
         .resources = speaker_resources,
         .resource_count = 2},
        {.name = "cmos-rtc",
         .parent = 0,
         .properties = rtc_properties,
         .property_count = 1,
         .resources = rtc_resources,
         .resource_count = 2},
    };
    static const fw_static_tree_t tree = {nodes,
                                          sizeof(nodes) / sizeof(nodes[0])};
    nodes[1].disabled = board_firmware_device_exists(0, speaker_id);
    nodes[2].disabled = board_firmware_device_exists(0, rtc_id);
    board_firmware_static_enumerate(&tree);
}
#endif
