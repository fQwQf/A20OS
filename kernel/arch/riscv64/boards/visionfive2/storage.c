/* JH7110 SDIO1 clock handoff. Public register facts: Linux v6.12
 * clk-starfive-jh7110-{sys,pll}.c and dt-bindings/clock/starfive,jh7110-crg.h.
 * Preserve the boot firmware's pinctrl/power/phase state at default SD speed.
 * Never assume PLL2 or the bus clock was programmed to one particular rate.
 */
#include "drivers/block/dw_sdio.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/klog.h"
#include "platform.h"

#define SYSCRG (0x13020000UL + PAGE_OFFSET)
#define SYSCON (0x13030000UL + PAGE_OFFSET)

static uint32_t reg(uintptr_t base, unsigned offset) {
    return readl((const volatile void *)(base + offset));
}
static uint64_t pll2_rate(void) {
    uint32_t control = reg(SYSCON, 0x2c), fraction = reg(SYSCON, 0x30);
    unsigned mode = (control >> 15) & 3;
    unsigned prediv = reg(SYSCON, 0x34) & 0x3f;
    unsigned fbdiv = (control >> 17) & 0xfff;
    unsigned postdiv = (fraction >> 28) & 3;
    if (!prediv || !fbdiv || (mode != 0 && mode != 3))
        return 0;
    uint64_t rate = 24000000ULL * fbdiv;
    if (!mode)
        rate += 24000000ULL * (fraction & 0xffffff) / (1U << 24);
    return rate / ((uint64_t)prediv << postdiv);
}
void vf2_configure_storage(void) {
    device_t *device = device_find_get_by_mmio(0x16020000);
    if (!device)
        return;
    if (fw_node_match_id(&device->firmware_node, "starfive,jh7110-mmc") != 1) {
        device_put(device);
        return;
    }
    /* BUS_ROOT mux: oscillator or PLL2. AXI_CFG0 divider is literal, not n-1. */
    uint64_t bus = (reg(SYSCRG, 5 * 4) & (1U << 24)) ? pll2_rate() : 24000000;
    unsigned axi_div = reg(SYSCRG, 7 * 4) & 3;
    uint64_t axi = axi_div ? bus / axi_div : 0;
    unsigned card_div = (unsigned)((axi + 49999999) / 50000000);
    if (!axi || card_div < 1 || card_div > 15) {
        kwarn("[VF2] SDIO1 invalid inherited clock tree; host stays "
              "unconfigured\n");
        device_put(device);
        return;
    }
    /* Only the SDIO1 local divider/gates are changed. Shared bus/PLL rates and
     * pinmux are owned by the boot firmware, which loaded this SD image. */
    uint32_t clock = reg(SYSCRG, 94 * 4);
    writel((clock & ~0xfU) | card_div | (1U << 31),
           (volatile void *)(SYSCRG + 94 * 4));
    writel(reg(SYSCRG, 92 * 4) | (1U << 31),
           (volatile void *)(SYSCRG + 92 * 4));
    /* JH7110_SYSRST_SDIO1_AHB = 65, reset word2 bit1, active high. */
    writel(reg(SYSCRG, 0x300) & ~(1U << 1), (volatile void *)(SYSCRG + 0x300));
    static dw_sdio_platform_data_t host;
    host.input_hz = (uint32_t)(axi / card_div);
    host.max_hz = 25000000;
    host.fifo_depth = 32;
    device->plat_data = &host;
    device_put(device);
}
