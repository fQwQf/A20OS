/* SPDX-License-Identifier: Apache-2.0
 * A64/V3s MMC0 electrical preparation, deliberately owned by arch.
 * Both SoCs use CCU BUS_GATE0[8]/BUS_RESET0[8], SDC0_CLK at +0x88,
 * and PF0..PF5 function 2. No PMIC state is changed: the boot SD supply
 * must remain enabled by SPL/U-Boot. Only the enabled firmware MMC0 node
 * is configured; secondary controllers and disabled nodes are untouched.
 */
#ifndef A20_ARCH_SUNXI_MMC0_H
#define A20_ARCH_SUNXI_MMC0_H
#include "drivers/block/sunxi_mmc.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/stdio.h"

static void sunxi_board_prepare_mmc0(int calibrate) {
    static sunxi_mmc_platform_t platform = {
        .clock_hz = 400000,
        .fifo_offset = 0x200,
    };
    device_t *device = device_find_get_by_mmio(0x01c0f000);
    if (!device)
        return;
    const char *id =
        calibrate ? "allwinner,sun50i-a64-mmc" : "allwinner,sun7i-a20-mmc";
    if (fw_node_match_id(&device->firmware_node, id) != 1) {
        device_put(device);
        return;
    }
    volatile uint32_t *ccu = arch_device_map_mmio(0x01c20000, 0x400, 0);
    volatile uint32_t *pio = arch_device_map_mmio(0x01c20800, 0x100, 0);
    if (!ccu || !pio) {
        if (ccu)
            arch_device_unmap_mmio((void *)ccu, 0x400);
        if (pio)
            arch_device_unmap_mmio((void *)pio, 0x100);
        printf("[SUNXI-MMC] MMC0 clock/pin registers cannot be mapped\n");
        device_put(device);
        return;
    }
    /* PF register bank stride 0x24, bank F index 5. Preserve PF6/7 and
     * all other pins; data/command/clock use function 2, 30mA, pull-up. */
    unsigned bank = 5U * 0x24U;
    writel((readl(pio + bank / 4) & ~0x00ffffffU) | 0x00222222U,
           pio + bank / 4);
    writel((readl(pio + (bank + 0x14) / 4) & ~0xfffU) | 0xaaaU,
           pio + (bank + 0x14) / 4);
    writel((readl(pio + (bank + 0x1c) / 4) & ~0xfffU) | 0x555U,
           pio + (bank + 0x1c) / 4);
    writel(readl(ccu + 0x60 / 4) | (1U << 8), ccu + 0x60 / 4);
    writel(readl(ccu + 0x2c0 / 4) | (1U << 8), ccu + 0x2c0 / 4);
    /* V3s: OSC24M / (2^2 * 15) = 400kHz. A64 has a fixed /2
     * post-divider, so its N is 1 instead. Neither PLL programming nor
     * voltage switching/tuning is needed at this initialization rate. */
    writel(0, ccu + 0x88 / 4);
    writel((1U << 31) | ((calibrate ? 1U : 2U) << 16) | 14U, ccu + 0x88 / 4);
    (void)readl(ccu + 0x88 / 4);
    platform.a64_timing = calibrate;
    device->plat_data = &platform;
    arch_device_unmap_mmio((void *)pio, 0x100);
    arch_device_unmap_mmio((void *)ccu, 0x400);
    device_put(device);
}
#endif
