/* SPDX-License-Identifier: Apache-2.0 */
#ifdef CONFIG_BOARD_OX64
#include "drivers/block/sdhci.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"

/* GLB clock/reset and GPIO0..5 are mechanisms of the BL808 board. The SD
 * protocol driver never assumes these addresses or touches the M0 interrupt
 * bridge: PIO deliberately does not require that bridge or DMA coherence. */
static int ox64_sd_prepare(device_t *dev, void *host) {
    (void)dev;
    (void)host;
    volatile unsigned char *glb = arch_device_map_mmio(0x20000000, 0x1000, 0);
    if (!glb)
        return -ENOMEM;
    writel(readl(glb + 0x584) | (1U << 22), glb + 0x584); /* AHB SDH gate */
    /* Select WIFIPLL/96MHz, divide by one. OpenBouffalo firmware establishes
     * WIFIPLL; SDHCI's own divider supplies <=400kHz during identification. */
    uint32_t value = readl(glb + 0x430);
    value = (value & ~((7U << 9) | (1U << 12))) | (1U << 13);
    writel(value, glb + 0x430);
    uint32_t reset = readl(glb + 0x540) & ~(1U << 22);
    writel(reset, glb + 0x540);
    udelay(10);
    writel(reset | (1U << 22), glb + 0x540);
    udelay(10);
    writel(reset, glb + 0x540);
    /* Function SDH=0, alternate input, pull-up, Schmitt, drive=1. Mask GPIO
     * interrupts and disable GPIO output override on these six dedicated pins. */
    for (unsigned pin = 0; pin < 6; ++pin) {
        unsigned offset = 0x8c4 + pin * 4;
        uint32_t pad = readl(glb + offset);
        pad &= ~((31U << 8) | (3U << 2) | (1U << 5) | (1U << 6) | (3U << 30));
        pad |= 1U | 2U | (1U << 2) | (1U << 4) | (1U << 22);
        writel(pad, glb + offset);
    }
    writel(readl(glb + 0x510) | (1U << 22), glb + 0x510); /* SDH GPIO0..5 */
    int rc = (readl(glb + 0x430) & ((7U << 9) | (1U << 12) | (1U << 13))) ==
                     (1U << 13)
                 ? 0
                 : -EIO;
    arch_device_unmap_mmio((void *)glb, 0x1000);
    return rc;
}
static int ox64_sd_before_idle(device_t *dev, void *mapped) {
    (void)dev;
    (void)mapped;
    /* Vendor registers lie beyond the old OpenBouffalo DT's 0x100-byte host
     * resource. Map that separate documented range in the board layer. */
    volatile unsigned char *vendor = arch_device_map_mmio(0x20060100, 0x20, 0);
    if (!vendor)
        return -ENOMEM;
    writel(readl(vendor + 0x18) | (1U << 30), vendor + 0x18); /* TX int clock */
    uint16_t misc = readw(vendor + 0x0e);
    writew(misc | 6U, vendor + 0x0e); /* enable misc status, W1C previous */
    uint32_t config = readl(vendor);
    config = (config & ~(0xffU << 24)) | (80U << 24);
    writel(config, vendor);
    writel(config | (1U << 6), vendor); /* generate 80 initial SD clocks */
    uint64_t start = clock_get_ticks(), limit = clock_ticks_per_sec() / 10;
    int rc = 0;
    while (!(readw(vendor + 0x0e) & 4U)) {
        if (clock_get_ticks() - start >= limit) {
            rc = -ETIMEDOUT;
            break;
        }
        udelay(10);
    }
    writew(readw(vendor + 0x0e) | 4U, vendor + 0x0e);
    arch_device_unmap_mmio((void *)vendor, 0x20);
    return rc;
}
static const sdhci_platform_data_t ox64_sd_platform = {
    .input_hz = 96000000,
    .max_hz = 25000000,
    /* BL808 has no SDHCI_HOST_VERSION; the verified Linux driver reports 0. */
    .spec_version = 0,
    .prepare = ox64_sd_prepare,
    .before_idle = ox64_sd_before_idle,
};
void ox64_configure_storage(void) {
    device_t *dev = device_find_get_by_mmio(0x20060000);
    if (!dev)
        return;
    if (!dev->user_owned && !dev->plat_data &&
        fw_node_match_id(&dev->firmware_node, "bflb,bl808-sdhci") > 0)
        dev->plat_data = (void *)&ox64_sd_platform;
    device_put(dev);
}
#endif
