/* SPDX-License-Identifier: Apache-2.0
 * Quartz64 Zero v0.1 wiring only: RK817 on I2C0/0x20, I2S1_M0, LDO4=3.3V.
 * Facts: board schematic20240428 sheets15/17/23, RK3568 TRM, upstream Linux
 * v6.17 clk-rk3568/io-domain/rk817_codec and U-Boot2026.07 pinctrl-rk3568.
 * Include from the board implementation after firmware enumeration.
 */
#ifndef A20_ARCH_RK3566_AUDIO_H
#define A20_ARCH_RK3566_AUDIO_H
#include "drivers/audio/rk817_audio.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/bus/rk3x_i2c_register.h"
#include "firmware/node.h"
#include "firmware/device.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/errno.h"
static rk817_audio_platform_t rk3566_audio_platform;
static mutex_t rk3566_audio_i2c_lock;
/* I2C0 is firmware-initialized. Preserve its clock divider and tuning and
 * reject any active controller/IRQ owner; this is not a second bus master. */
static int rk3566_codec_register(uint8_t reg, uint8_t *value, int write) {
    mutex_lock(&rk3566_audio_i2c_lock);
    volatile uint8_t *io =
        arch_device_map_mmio(rk3566_audio_platform.provider_pa[0], 0x1000, 0);
    int r = io ? rk3x_i2c_register(io, 0x20, reg, value, write) : -ENOMEM;
    if (io)
        arch_device_unmap_mmio((void *)io, 0x1000);
    mutex_unlock(&rk3566_audio_i2c_lock);
    return r;
}
static void rk3566_audio_mask(volatile uint8_t *io, unsigned reg, uint32_t mask,
                              uint32_t value) {
    writel((mask << 16) | (value & mask), io + reg);
}
static uint64_t rk3566_audio_gcd(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t n = a % b;
        a = b;
        b = n;
    }
    return a;
}
static int rk3566_audio_prepare(void) {
    volatile uint8_t *cru =
        arch_device_map_mmio(rk3566_audio_platform.provider_pa[1], 0x1000, 0);
    volatile uint8_t *grf =
        arch_device_map_mmio(rk3566_audio_platform.provider_pa[2], 0x1000, 0);
    volatile uint8_t *pmucru =
        arch_device_map_mmio(rk3566_audio_platform.provider_pa[3], 0x1000, 0);
    volatile uint8_t *i2s = arch_device_map_mmio(0xfe410000, 0x1000, 0);
    volatile uint8_t *pmugrf =
        arch_device_map_mmio(rk3566_audio_platform.provider_pa[4], 0x1000, 0);
    int r = -ENOMEM;
    if (!cru || !grf || !pmucru || !pmugrf || !i2s)
        goto done;
    /* Do not retune shared GPLL (HDMI and other devices depend on it). Read
     * its actual integer-mode factors and derive an exact16-bit fraction. */
    uint32_t mode = (readl(cru + 0xc0) >> 6) & 3;
    uint32_t con0 = readl(cru + 0x40), con1 = readl(cru + 0x44);
    uint32_t ref = con1 & 63, post1 = (con0 >> 12) & 7, post2 = (con1 >> 6) & 7;
    r = -EINVAL;
    if (mode != 1 || !ref || !post1 || !post2 || !(con1 & (1U << 12)) ||
        !(con1 & (1U << 10)))
        goto done;
    uint64_t parent = 24000000ULL * (con0 & 4095) / (ref * post1 * post2);
    uint64_t divisor = rk3566_audio_gcd(parent, 12288000),
             m = 12288000 / divisor, n = parent / divisor;
    if (!parent || m > 65535 || n > 65535 || parent < 20 * 12288000ULL)
        goto done;
    /* Enable register access without retuning a possibly active stream. */
    rk3566_audio_mask(cru, 0x314, 1U << 11, 0);
    if (readl(i2s + 0x1c) & 3) {
        r = -EBUSY;
        goto done;
    }
    rk3566_audio_mask(pmucru, 0x184, 3, 0); /* I2C0 pclk+module gates */
    uint8_t idhi = 0, idlo = 0, enable = 0, voltage = 0;
    r = rk3566_codec_register(0xed, &idhi, 0);
    if (r < 0)
        goto done;
    r = rk3566_codec_register(0xee, &idlo, 0);
    if (r < 0)
        goto done;
    if ((((unsigned)idhi << 8) | idlo) >> 4 != 0x817) {
        r = -ENODEV;
        goto done;
    }
    r = rk3566_codec_register(0xb2, &enable, 0);
    if (r < 0)
        goto done;
    r = rk3566_codec_register(0xd2, &voltage, 0);
    if (r < 0)
        goto done;
    /* The prepared board firmware owns this shared IO rail. Require the
     * exact3.3V LDO4 contract; never reprogram an active SoC IO supply. */
    if (!(enable & 8) || (voltage & 127) != 108) {
        r = -EACCES;
        goto done;
    }
    /* VCCIO1 3.3V selected before driving audio pins. Preserve every other
     * voltage domain and all video clock, reset and regulator fields. */
    rk3566_audio_mask(pmugrf, 0x140, 2, 0);
    rk3566_audio_mask(pmugrf, 0x144, 2, 2);
    rk3566_audio_mask(grf, 0x310, 3U << 10, 0);    /* I2S1 route M0 */
    rk3566_audio_mask(grf, 0x000, 0xff00, 0x1100); /* GPIO1_A2/A3 func1 */
    rk3566_audio_mask(grf, 0x004, 0xf0f0, 0x1010); /* GPIO1_A5/A7 func1 */
    rk3566_audio_mask(grf, 0x504, 1U << 5, 1U << 5);
    rk3566_audio_mask(grf, 0x508, 2, 2);
    /* Source GPLL/1, fractional path, mclkout from TX. Stop only our audio
     * gates while changing the fraction, then enable all four TX gates. */
    rk3566_audio_mask(cru, 0x318, 0xf00, 0xf00);
    rk3566_audio_mask(cru, 0x13c, 0x8f7f, 1U << 10);
    writel(((uint32_t)m << 16) | (uint32_t)n, cru + 0x140);
    rk3566_audio_mask(cru, 0x318, 0xf00, 0);
    rk3566_audio_mask(cru, 0x314, 1U << 11, 0); /* HCLK_I2S1 */
    rk3566_audio_mask(cru, 0x368, 1, 0);        /* ACLK_BUS: PL330 */
    rk3566_audio_mask(cru, 0x414, 3U << 2, 0);  /* deassert I2S1 TX/RX resets */
    r = readl(cru + 0x140) == (((uint32_t)m << 16) | (uint32_t)n) &&
                (readl(cru + 0x13c) & 0x8f7f) == (1U << 10) &&
                !(readl(pmugrf + 0x140) & 2) && (readl(pmugrf + 0x144) & 2)
            ? 0
            : -EIO;
done:
    if (i2s)
        arch_device_unmap_mmio((void *)i2s, 0x1000);
    if (cru)
        arch_device_unmap_mmio((void *)cru, 0x1000);
    if (grf)
        arch_device_unmap_mmio((void *)grf, 0x1000);
    if (pmucru)
        arch_device_unmap_mmio((void *)pmucru, 0x1000);
    if (pmugrf)
        arch_device_unmap_mmio((void *)pmugrf, 0x1000);
    return r;
}
static int rk3566_audio_find(uint64_t address, size_t size, const char *id,
                             device_t **out) {
    device_t *d = device_find_get_by_mmio(address);
    if (!d)
        return -ENODEV;
    resource_t *r = device_get_resource(d, RES_MMIO, 0);
    if (!r || r->start != address || r->end < r->start ||
        r->end - r->start < size - 1 ||
        fw_node_match_id(&d->firmware_node, id) != 1) {
        device_put(d);
        return -ENODEV;
    }
    *out = d;
    return 0;
}
static int rk3566_audio_codec_node(const fw_node_t *i2c, const fw_node_t *cru) {
    fw_node_t child, previous;
    int have_previous = 0;
    while (!fw_node_next_child(i2c, have_previous ? &previous : NULL, &child)) {
        previous = child;
        have_previous = 1;
        uint32_t address = 0;
        if (fw_node_available(&child) != 1 ||
            fw_node_match_id(&child, "rockchip,rk817") != 1 ||
            fw_node_read_u32(&child, "reg", &address) || address != 0x20)
            continue;
        fw_reference_t clock;
        const char *clock_name = NULL;
        uint32_t cells = 1;
        if (fw_node_read_u32(&child, "#sound-dai-cells", &cells) || cells ||
            fw_node_read_string(&child, "clock-names", 0, &clock_name) ||
            strcmp(clock_name, "mclk") ||
            fw_node_reference(&child, "clocks", "#clock-cells", 0, &clock) ||
            clock.count != 1 || clock.args[0] != 72 ||
            !fw_node_equal(&clock.node, cru))
            return -EINVAL;
        fw_node_t codec, prev;
        int had = 0;
        while (!fw_node_next_child(&child, had ? &prev : NULL, &codec)) {
            prev = codec;
            had = 1;
            const char *name = fw_node_name(&codec);
            if (name && !strcmp(name, "codec") &&
                fw_node_available(&codec) == 1)
                return 0;
        }
    }
    return -ENODEV;
}
/* Caller selects the real Quartz64-Zero topology, never an A/B regulator map.
 * Disabled/malformed firmware descriptions produce no MMIO or power writes. */
static int rk3566_board_prepare_audio(void) {
    const struct {
        uint64_t pa;
        size_t size;
        const char *id;
    } specs[] = {{0xfe410000, 0x1000, "rockchip,rk3568-i2s-tdm"},
                 {0xfe550000, 0x4000, "arm,pl330"},
                 {0xfdd40000, 0x1000, "rockchip,rk3568-i2c"},
                 {0xfdd20000, 0x1000, "rockchip,rk3568-cru"},
                 {0xfdc60000, 0x1000, "rockchip,rk3568-grf"},
                 {0xfdd00000, 0x1000, "rockchip,rk3568-pmucru"},
                 {0xfdc20000, 0x1000, "rockchip,rk3568-pmugrf"}};
    device_t *nodes[7] = {0};
    int r = 0;
    for (unsigned i = 0; i < 7; i++) {
        r = rk3566_audio_find(specs[i].pa, specs[i].size, specs[i].id,
                              &nodes[i]);
        if (r < 0)
            goto done;
    }
    if (nodes[0]->plat_data || nodes[1]->drv || nodes[2]->drv) {
        r = -EBUSY;
        goto done;
    }
    r = rk3566_audio_codec_node(&nodes[2]->firmware_node,
                                &nodes[3]->firmware_node);
    if (r < 0)
        goto done;
    rk817_audio_platform_t platform = {.magic = RK817_AUDIO_MAGIC,
                                       .dma_pa = specs[1].pa,
                                       .dma_request = 2,
                                       .prepare = rk3566_audio_prepare,
                                       .codec_register = rk3566_codec_register};
    for (unsigned i = 0; i < 2; i++) {
        int irq = firmware_device_irq(nodes[1], i);
        if (irq <= 0) {
            r = irq < 0 ? irq : -EINVAL;
            goto done;
        }
        platform.dma_irq[i] = (uint32_t)irq;
    }
    for (unsigned i = 0; i < 5; i++)
        platform.provider_pa[i] = specs[i + 2].pa;
    rk3566_audio_platform = platform;
    mutex_init(&rk3566_audio_i2c_lock);
    nodes[0]->plat_data = &rk3566_audio_platform;
done:
    for (unsigned i = 0; i < 7; i++)
        if (nodes[i])
            device_put(nodes[i]);
    return r;
}
#endif
