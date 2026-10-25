/* SPDX-License-Identifier: Apache-2.0 */
#ifdef CONFIG_BOARD_LICHEERV_NANO
#include "drivers/block/sdhci.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"

/* SG2002 shares the CV18xx SD clock gate/bypass and SD0 pin registers.
 * Use the 25MHz oscillator bypass rather than guessing a bootloader PLL rate.
 * Linux clk-cv1800.c and pinctrl-sg2002.c document these fields. */
static int nano_sd_prepare(device_t *dev, void *host) {
    (void)dev;
    (void)host;
    volatile unsigned char *clock = arch_device_map_mmio(0x03002000, 0x1000, 0);
    if (!clock)
        return -ENOMEM;
    uint32_t bypass = readl(clock + 0x30) | (1U << 6);
    writel(bypass, clock + 0x30);
    uint32_t enabled = readl(clock) | (1U << 18) | (1U << 19);
    writel(enabled, clock);
    int rc = ((readl(clock + 0x30) & (1U << 6)) &&
              (readl(clock) & (3U << 18)) == (3U << 18))
                 ? 0
                 : -EIO;
    arch_device_unmap_mmio((void *)clock, 0x1000);
    if (rc)
        return rc;
    volatile unsigned char *pins = arch_device_map_mmio(0x03001000, 0x1000, 0);
    if (!pins)
        return -ENOMEM;
    for (unsigned offset = 0x1c; offset <= 0x30; offset += 4) {
        writel(readl(pins + offset) & ~7U, pins + offset);
        if (readl(pins + offset) & 7U)
            rc = -EIO;
    }
    /* SD0_PWR_EN function 0 lets the host power-control register drive power. */
    writel(readl(pins + 0x38) & ~3U, pins + 0x38);
    arch_device_unmap_mmio((void *)pins, 0x1000);
    return rc;
}
static int nano_sd_before_idle(device_t *dev, void *mapped) {
    (void)dev;
    volatile unsigned char *host = mapped;
    /* DWCMSHC vendor area pointer, bounded by the published 4KiB resource. */
    unsigned vendor = readl(host + 0xe8) & 0xfff;
    if (vendor < 0x100 || vendor > 0xfb0 || (vendor & 3))
        return -EINVAL;
    uint16_t clock = readw(host + 0x2c);
    writew(clock & ~4U, host + 0x2c);
    /* Native SD mode, 1T latency and bypassed PHY delay for default speed.
     * SG2002 reset sequence in upstream sdhci-of-dwcmshc.c. */
    writel((readl(host + vendor) & ~1U) | 2U, host + vendor);
    writel(readl(host + vendor + 0x4c) | 1U, host + vendor + 0x4c);
    writel((1U << 8) | (1U << 24), host + vendor + 0x40);
    writew(clock, host + 0x2c);
    return 0;
}
static const sdhci_platform_data_t nano_sd_platform = {
    .input_hz = 25000000,
    .max_hz = 25000000,
    .spec_version = 2,
    .prepare = nano_sd_prepare,
    .before_idle = nano_sd_before_idle,
};
void licheerv_nano_configure_storage(void) {
    device_t *dev = device_find_get_by_mmio(0x04310000);
    if (!dev)
        return;
    if (!dev->user_owned && !dev->plat_data &&
        (fw_node_match_id(&dev->firmware_node, "sophgo,cv1800b-dwcmshc") > 0 ||
         fw_node_match_id(&dev->firmware_node, "sophgo,sg2002-dwcmshc") > 0))
        dev->plat_data = (void *)&nano_sd_platform;
    device_put(dev);
}
#endif
