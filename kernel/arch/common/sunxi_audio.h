/* SPDX-License-Identifier: Apache-2.0
 * Original PINE A64 / V3s analog-audio electrical topology. Register facts:
 * Linux v6.17 ccu-sun50i-a64.c, ccu-sun8i-v3s.c; U-Boot v2025.10
 * sun8i_rsb.c, prcm_sun6i.h, axp_regulator.c (AXP803 ELDO1: 50mV steps).
 * Include only from a board implementation; no architecture test enters the
 * common PCM driver. Passing connected=0 leaves clocks, supplies and DT alone.
 */
#ifndef A20_ARCH_SUNXI_AUDIO_H
#define A20_ARCH_SUNXI_AUDIO_H
#include "drivers/audio/sunxi_audio.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/platform_bus.h"
#include "core/errno.h"
#include "core/stdio.h"

static sunxi_audio_platform_t sunxi_audio_platform;
static uint64_t sunxi_audio_ccu_pa, sunxi_audio_rsb_pa;
static int sunxi_audio_supply_changed;
static uint8_t sunxi_audio_old_eldo1;

static uint32_t sunxi_audio_mmio_read(volatile uint8_t *base, unsigned reg) {
    return readl(base + reg);
}
static void sunxi_audio_mmio_write(volatile uint8_t *base, unsigned reg, uint32_t value) {
    writel(value, base + reg);
}
static int sunxi_audio_rsb_wait(volatile uint8_t *rsb) {
    for (unsigned i = 0; i < 10000; i++) {
        uint32_t status = sunxi_audio_mmio_read(rsb, 0x0c);
        if (status & 7) {
            sunxi_audio_mmio_write(rsb, 0x0c, status & 7);
            return status & 6 ? -EIO : 0;
        }
        udelay(10);
    }
    return -ETIMEDOUT;
}
static int sunxi_audio_rsb_transfer(volatile uint8_t *rsb, unsigned reg,
                                  uint8_t *value, int write) {
    sunxi_audio_mmio_write(rsb, 0x0c, 7);
    sunxi_audio_mmio_write(rsb, 0x30, 0x2dU << 16);
    sunxi_audio_mmio_write(rsb, 0x10, reg);
    if (write) sunxi_audio_mmio_write(rsb, 0x1c, *value);
    sunxi_audio_mmio_write(rsb, 0x2c, write ? 0x4e : 0x8b);
    sunxi_audio_mmio_write(rsb, 0x00, 1U << 7);
    int result = sunxi_audio_rsb_wait(rsb);
    if (!result && !write) *value = sunxi_audio_mmio_read(rsb, 0x1c) & 255;
    return result;
}
/* PINE's headphone charge pump requires ELDO1=1.8V, unlike V3s' externally
 * supplied analog block. Use the SPL/TF-A initialized RSB bus; never reset a
 * live PMIC bus or reassign its address behind firmware's back. Validate both
 * chip identity and write readback, preserve other regulator-enable bits. */
static int sunxi_audio_power(int on) {
    if (sunxi_audio_platform.variant == SUNXI_AUDIO_V3S) return 0;
    volatile uint8_t *rsb = arch_device_map_mmio(sunxi_audio_rsb_pa, 0x40, 0);
    if (!rsb) return -ENOMEM;
    int result = 0;
    uint8_t id = 0, enable = 0, voltage = 0;
    if (sunxi_audio_mmio_read(rsb, 0) & (1U << 7)) { result = -EBUSY; goto done; }
    result = sunxi_audio_rsb_transfer(rsb, 0x03, &id, 0);
    if (result < 0 || (id & 0xcf) != 0x41) { if (!result) result = -ENODEV; goto done; }
    result = sunxi_audio_rsb_transfer(rsb, 0x12, &enable, 0);
    if (result < 0) goto done;
    result = sunxi_audio_rsb_transfer(rsb, 0x19, &voltage, 0);
    if (result < 0) goto done;
    if (on) {
        /* Never retune a live ELDO1 consumer (e.g. optional SDIO Wi-Fi). */
        if ((enable & 1) && (voltage & 31) != 22) { result = -EBUSY; goto done; }
        if (enable & 1) goto done;
        sunxi_audio_old_eldo1 = voltage;
        sunxi_audio_supply_changed = 1;
        uint8_t desired = (voltage & ~31U) | 22U;
        result = sunxi_audio_rsb_transfer(rsb, 0x19, &desired, 1);
        if (result < 0) goto done;
        result = sunxi_audio_rsb_transfer(rsb, 0x19, &voltage, 0);
        if (result < 0 || (voltage & 31) != 22) { if (!result) result = -EIO; goto done; }
        enable |= 1;
        result = sunxi_audio_rsb_transfer(rsb, 0x12, &enable, 1);
        if (result < 0) goto done;
        sunxi_audio_supply_changed = 1;
        result = sunxi_audio_rsb_transfer(rsb, 0x12, &enable, 0);
        if (!result && !(enable & 1)) result = -EIO;
        if (!result) mdelay(10);
    } else if (sunxi_audio_supply_changed) {
        enable &= ~1U;
        result = sunxi_audio_rsb_transfer(rsb, 0x12, &enable, 1);
        if (!result) result = sunxi_audio_rsb_transfer(rsb, 0x19, &sunxi_audio_old_eldo1, 1);
        if (!result) sunxi_audio_supply_changed = 0;
    }
done:
    arch_device_unmap_mmio((void *)rsb, 0x40);
    return result;
}
static int sunxi_audio_clocks(void) {
    volatile uint8_t *ccu = arch_device_map_mmio(sunxi_audio_ccu_pa, 0x400, 0);
    if (!ccu) return -ENOMEM;
    const uint32_t mask = (127U << 8) | 31U | (15U << 16) | (1U << 24);
    const uint32_t desired = (13U << 8) | 13U | (1U << 24);
    uint32_t old = sunxi_audio_mmio_read(ccu, 0x008);
    uint32_t old_pattern = sunxi_audio_mmio_read(ccu, 0x284);
    int result = 0;
    /* A different running PLL can belong to HDMI/I2S. Refuse takeover. */
    if ((old & (1U << 31)) && ((old & mask) != desired ||
        sunxi_audio_mmio_read(ccu, 0x284) != 0xc000ac02U)) { result = -EBUSY; goto done; }
    sunxi_audio_mmio_write(ccu, 0x284, 0xc000ac02U);
    sunxi_audio_mmio_write(ccu, 0x008, (old & ~mask) | desired | (1U << 31));
    unsigned attempt;
    for (attempt = 0; attempt < 10000; attempt++) {
        if (sunxi_audio_mmio_read(ccu, 0x008) & (1U << 28)) break;
        udelay(10);
    }
    if (attempt == 10000) {
        sunxi_audio_mmio_write(ccu, 0x008, old);
        sunxi_audio_mmio_write(ccu, 0x284, old_pattern);
        result = -ETIMEDOUT;
        goto done;
    }
    /* CCU BUS_GATE0/BUS_RESET0[6]=DMA; APB1 gate/reset[0]=codec. Do not
     * assert a global reset while another DMA channel may own hardware. */
    sunxi_audio_mmio_write(ccu, 0x060, sunxi_audio_mmio_read(ccu, 0x060) | (1U << 6));
    sunxi_audio_mmio_write(ccu, 0x2c0, sunxi_audio_mmio_read(ccu, 0x2c0) | (1U << 6));
    sunxi_audio_mmio_write(ccu, 0x068, sunxi_audio_mmio_read(ccu, 0x068) | 1U);
    sunxi_audio_mmio_write(ccu, 0x2d0, sunxi_audio_mmio_read(ccu, 0x2d0) | 1U);
    sunxi_audio_mmio_write(ccu, 0x140, sunxi_audio_mmio_read(ccu, 0x140) | (1U << 31));
done:
    arch_device_unmap_mmio((void *)ccu, 0x400);
    return result;
}
/* Lookup may return a containing resource: require the actual IP block, a
 * complete register window and a matching entry anywhere in compatible[]. */
static int sunxi_audio_resource(uint64_t address, size_t minimum,
                                const char *compatible, device_t **out) {
    device_t *device = device_find_get_by_mmio(address);
    if (!device) return -ENODEV;
    resource_t *r = device_get_resource(device, RES_MMIO, 0);
    if (!r || r->start != address || r->end < r->start ||
        r->end - r->start < minimum - 1 ||
        fw_node_match_id(&device->firmware_node, compatible) != 1) {
        device_put(device);
        return -ENODEV;
    }
    *out = device;
    return 0;
}
/* Call from configure_devices after firmware enumeration. PINE A64 passes
 * A64,1; V3s must pass V3S,1 ONLY for an explicitly selected Dock/external HP
 * wiring and an enabled matching codec DT node. The bare Zero defaults to0. */
static int sunxi_board_prepare_audio(unsigned variant, int connected) {
    if (!connected) return 0;
    if (variant != SUNXI_AUDIO_A64 && variant != SUNXI_AUDIO_V3S) return -EINVAL;
    int a64 = variant == SUNXI_AUDIO_A64;
    struct {
        uint64_t address;
        size_t minimum;
        const char *compatible;
    } blocks[] = {
        {0x01c22c00, 0x200, a64 ? "allwinner,sun50i-a64-codec-i2s" : "allwinner,sun8i-v3s-codec"},
        {0x01c02000, 0x1000, a64 ? "allwinner,sun50i-a64-dma" : "allwinner,sun8i-v3s-dma"},
        {0x01c20000, 0x400, a64 ? "allwinner,sun50i-a64-ccu" : "allwinner,sun8i-v3s-ccu"},
        {a64 ? 0x01f015c0 : 0x01c23000, 4, a64 ? "allwinner,sun50i-a64-codec-analog" : "allwinner,sun8i-v3s-codec-analog"},
        {0x01c22e00, 0x600, "allwinner,sun50i-a64-codec"},
        {0x01f03400, 0x40, "allwinner,sun8i-a23-rsb"},
    };
    device_t *devices[6] = {0};
    unsigned count = a64 ? 6 : 4;
    int result = 0;
    for (unsigned i = 0; i < count; i++) {
        result = sunxi_audio_resource(blocks[i].address, blocks[i].minimum,
                                      blocks[i].compatible, &devices[i]);
        if (result < 0) goto done;
    }
    if (devices[0]->plat_data) { result = -EBUSY; goto done; }
    int irq = platform_device_irq(devices[1]);
    if (irq <= 0) { result = irq < 0 ? irq : -EINVAL; goto done; }
    /* Configuration publishes only validated resources. The driver registers
     * every provider dependency during probe, because the core clears links
     * before each attempt. No MMIO or power side effects occur here. */
    sunxi_audio_platform = (sunxi_audio_platform_t){
        .magic = SUNXI_AUDIO_PLATFORM_MAGIC, .variant = variant,
        .dma_pa = blocks[1].address, .analog_pa = blocks[3].address,
        .digital_pa = a64 ? blocks[4].address : 0, .dma_irq = (uint32_t)irq,
        .ccu_pa = blocks[2].address, .rsb_pa = a64 ? blocks[5].address : 0,
        .prepare = sunxi_audio_clocks, .power = sunxi_audio_power,
    };
    sunxi_audio_ccu_pa = blocks[2].address;
    sunxi_audio_rsb_pa = a64 ? blocks[5].address : 0;
    devices[0]->plat_data = &sunxi_audio_platform;
done:
    for (unsigned i = 0; i < count; i++) if (devices[i]) device_put(devices[i]);
    return result;
}
#endif
