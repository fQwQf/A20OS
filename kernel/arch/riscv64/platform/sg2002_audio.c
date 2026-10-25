/* SPDX-License-Identifier: Apache-2.0
 * Nano rev1.4: U11 PA enable is pulled high through R36, no mute GPIO.
 * SG200x SDK d4003f15 supplies exact clocks, DMA request and DAC reset facts.
 */
#include "sg2002_audio.h"
#include "dma_cache.h"
#include "boot/context.h"
#include "drivers/audio/sg2002_audio.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "firmware/device.h"
#include "core/errno.h"
#include "sg2002_audio_clock.h"

static sg2002_audio_platform_t platform;
static volatile uint8_t *top, *clocks, *reset, *subsystem, *pins, *dma;
static uint32_t rd(volatile uint8_t *p, unsigned r) {
    return readl(p + r);
}
static void set(volatile uint8_t *p, unsigned r, uint32_t mask, uint32_t v) {
    writel((rd(p, r) & ~mask) | v, p + r);
}
static int prepare(void) {
    int rc = riscv_sg2002_dma_cache_prepare();
    if (rc)
        return rc;
    /* Probe has retained every provider before these permanent boot mappings. */
    dma = arch_device_map_mmio(platform.provider_pa[1],
                               platform.provider_bytes[1], 0);
    top = arch_device_map_mmio(platform.provider_pa[2],
                               platform.provider_bytes[2], 0);
    clocks = arch_device_map_mmio(platform.provider_pa[3],
                                  platform.provider_bytes[3], 0);
    reset = arch_device_map_mmio(platform.provider_pa[4],
                                 platform.provider_bytes[4], 0);
    subsystem = arch_device_map_mmio(platform.provider_pa[5],
                                     platform.provider_bytes[5], 0);
    pins = arch_device_map_mmio(platform.provider_pa[6],
                                platform.provider_bytes[6], 0);
    if (!top || !clocks || !reset || !subsystem || !pins || !dma)
        return -ENODEV;
    volatile uint8_t *i2s = arch_device_map_mmio(platform.i2s_pa, 0x100, 0);
    volatile uint8_t *dac = arch_device_map_mmio(platform.dac_pa, 0x100, 0);
    if (!i2s || !dac)
        return -ENODEV;
    /* This driver configures a new stream, it does not adopt live firmware
     * audio. Refuse before touching PLL dividers, reset or routing. */
    if ((rd(i2s, 0x18) & 1) || (rd(dac, 0) & 3))
        return -EBUSY;
    if (readq(dma + 0x18) & (1ULL << 7))
        return -EBUSY;
    /* Native analog pad function only; no guessed digital pin selection. */
    if (rd(pins, 0x12c) & 7)
        return -EOPNOTSUPP;
    if (rd(clocks, 0x804) & 2)
        return -EBUSY;
    unsigned div = sg2002_audio_divider(rd(clocks, 0x808), rd(clocks, 0x854),
                                        rd(clocks, 0x840), rd(clocks, 0x80c));
    if (!div)
        return -EOPNOTSUPP;
    set(clocks, 0xa4, 0x00ff0308, (div << 16) | 8); /* AUD3 from A0PLL */
    set(clocks, 0x30, 1U << 14, 0);                 /* bypass off */
    set(clocks, 0x04, (1U << 1) | (1U << 5) | (1U << 27),
        (1U << 1) | (1U << 5) | (1U << 27));
    /* Preserve every other channel's remap and IRQ destination. */
    set(top, 0x158, 0xff000000U,
        0x87000000U); /* request7 -> DMA channel7 + update */
    set(top, 0x298, (1U << 17) | (1U << 18), (1U << 17) | (1U << 18));
    for (unsigned r = 0; r < 16; r += 4)
        set(subsystem, r, 0xf000, 0x7000);
    set(subsystem, 0x20, 1U << 3, 0); /* no cross-I2S synchronization */
    /* DAC active-low reset, never reset any unrelated codec or DMA block. */
    set(reset, 8, 1U << 27, 0);
    udelay(10);
    set(reset, 8, 1U << 27, 1U << 27);
    return 0;
}
static device_t *resource_device(uint64_t pa, size_t bytes) {
    device_t *d = device_find_get_by_mmio(pa);
    if (!d)
        return NULL;
    resource_t *r = device_get_resource(d, RES_MMIO, 0);
    if (!r || r->start != pa || r->end < r->start ||
        r->end - r->start < bytes - 1) {
        device_put(d);
        return NULL;
    }
    return d;
}
void sg2002_audio_configure(void) {
    const fw_node_t *root = &boot_context_get()->hardware_root;
    if (fw_node_match_id(root, "sophgo,sg2002") != 1 ||
        fw_node_match_id(root, "sipeed,licheerv-nano") != 1)
        return;
    device_t *consumer = resource_device(0x04130000, 0x100);
    if (!consumer)
        return;
    if (consumer->plat_data ||
        fw_node_match_id(&consumer->firmware_node, "cvitek,cv1835-i2s") != 1) {
        device_put(consumer);
        return;
    }
    uint64_t addresses[] = {0x0300a000, 0x04330000, 0x03000000, 0x03002000,
                            0x03003000, 0x04108000, 0x03001000};
    size_t sizes[] = {0x100, 0x1000, 0x1000, 0x1000, 0x100, 0x100, 0x1000};
    device_t *providers[7] = {0};
    int ok = 1;
    for (unsigned i = 0; i < 7; i++) {
        providers[i] = resource_device(addresses[i], sizes[i]);
        if (!providers[i])
            ok = 0;
    }
    if (ok && fw_node_match_id(&providers[0]->firmware_node,
                               "cvitek,cv182xadac") != 1)
        ok = 0;
    if (ok &&
        fw_node_match_id(&providers[1]->firmware_node, "snps,dmac-bm") != 1)
        ok = 0;
    if (ok && (providers[0]->drv || providers[0]->user_owned ||
               providers[1]->drv || providers[1]->user_owned))
        ok = 0;
    int irq = ok ? firmware_device_irq(providers[1], 0) : -ENODEV;
    if (irq <= 0)
        ok = 0;
    if (ok) {
        platform = (sg2002_audio_platform_t){
            .magic = SG2002_AUDIO_PLATFORM_MAGIC,
            .dma_irq = (uint32_t)irq,
            .i2s_pa = 0x04130000,
            .dac_pa = 0x0300a000,
            .dma_pa = 0x04330000,
            .prepare = prepare,
        };
        for (unsigned i = 0; i < SG2002_AUDIO_PROVIDERS; i++) {
            platform.provider_pa[i] = addresses[i];
            platform.provider_bytes[i] = sizes[i];
        }
        consumer->plat_data = &platform;
    }
    for (unsigned i = 0; i < 7; i++)
        if (providers[i])
            device_put(providers[i]);
    device_put(consumer);
}
