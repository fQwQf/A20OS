#include "core/string.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"

static platform_device_t g_rtc_pdev;
static platform_device_t g_vinput_pdev;

void board_register_module_devices(void) {
    /* goldfish RTC (dual-placement: kernel probe module + user rtcd). */
    static resource_t rtc_res[] = {
        {RES_MMIO, 0x101000, 0x1010FF, IORESOURCE_MMIO_32BIT, "rtc"},
    };
    memset(&g_rtc_pdev, 0, sizeof(g_rtc_pdev));
    g_rtc_pdev.dev.name = "goldfish-rtc";
    g_rtc_pdev.dev.res = rtc_res;
    g_rtc_pdev.dev.res_count = 1;
    g_rtc_pdev.dev.user_owned = 1;
    g_rtc_pdev.id.vendor = 0x101000;
    g_rtc_pdev.id.device = 0;
    g_rtc_pdev.id.subvendor = VENDOR_ANY;
    g_rtc_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_rtc_pdev);

    static resource_t vinput_res[] = {
        {RES_MMIO, 0x0A000A00, 0x0A000BFF, IORESOURCE_MMIO_32BIT, "vinput"},
    };
    memset(&g_vinput_pdev, 0, sizeof(g_vinput_pdev));
    g_vinput_pdev.dev.name = "virtio-input-slot5";
    g_vinput_pdev.dev.res = vinput_res;
    g_vinput_pdev.dev.res_count = 1;
    g_vinput_pdev.dev.user_owned = 1;
    g_vinput_pdev.id.vendor = 0x0A000A00;
    g_vinput_pdev.id.device = 0;
    g_vinput_pdev.id.subvendor = VENDOR_ANY;
    g_vinput_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_vinput_pdev);
}
