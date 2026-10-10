#include "core/string.h"
#include "core/arch.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"

static platform_device_t g_ps2_pdev;
static platform_device_t g_tpm_pdev;

void board_register_module_devices(void) {
    memset(&g_ps2_pdev, 0, sizeof(g_ps2_pdev));
    g_ps2_pdev.dev.name = "ps2";
    g_ps2_pdev.id.vendor = 0x50533200;
    g_ps2_pdev.id.device = 0;
    g_ps2_pdev.id.subvendor = VENDOR_ANY;
    g_ps2_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_ps2_pdev);

    uint64_t tpm_phys = firmware_acpi_tpm2();
    if (!tpm_phys)
        return;
    static resource_t tpm_res;
    tpm_res = (resource_t){RES_MMIO, tpm_phys, tpm_phys + 0x4fff, 0, "tpm-tis"};
    memset(&g_tpm_pdev, 0, sizeof(g_tpm_pdev));
    g_tpm_pdev.dev.name = "tpm";
    g_tpm_pdev.dev.res = &tpm_res;
    g_tpm_pdev.dev.res_count = 1;
    g_tpm_pdev.id.vendor = 0x54504D00;
    g_tpm_pdev.id.device = 0;
    g_tpm_pdev.id.subvendor = VENDOR_ANY;
    g_tpm_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_tpm_pdev);
}
