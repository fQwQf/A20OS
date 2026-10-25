/* RK3566 SDMMC0 board wiring. Register facts: Linux v6.12 clk-rk3568.c,
 * U-Boot v2025.10 pinctrl-rk3568.c and upstream rk3568-pinctrl.dtsi.
 * All writes use Rockchip's upper-half write mask, preserving unrelated pins.
 * The Zero socket is wired directly to VCC3V3_SYS: no regulator GPIO toggles.
 */
#include "drivers/block/dw_sdio.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/timer.h"
#include "platform.h"

#define CRU_BASE (0xfdd20000UL + PAGE_OFFSET)
#define GRF_BASE (0xfdc60000UL + PAGE_OFFSET)
#define PMUGRF_BASE (0xfdc20000UL + PAGE_OFFSET)

static void masked(uintptr_t base, unsigned offset, uint32_t mask,
                   uint32_t value) {
    writel((mask << 16) | (value & mask), (volatile void *)(base + offset));
}

void quartz64_zero_configure_storage(void) {
    device_t *device = device_find_get_by_mmio(0xfe2b0000);
    if (!device)
        return; /* Disabled or absent firmware nodes stay absent. */
    if (fw_node_match_id(&device->firmware_node, "rockchip,rk3568-dw-mshc") !=
        1) {
        device_put(device);
        return;
    }
    /* GPIO1 D5..D7: D0..D2; GPIO2 A0..A2: D3, CMD, CLK. */
    masked(GRF_BASE, 0x01c, 0xfff0, 0x1110);
    masked(GRF_BASE, 0x020, 0x0fff, 0x0111);
    masked(GRF_BASE, 0x08c, 0xfc00, 0x5400); /* Pull up data0..2. */
    masked(GRF_BASE, 0x090, 0x003f, 0x0015); /* Pull up data3/CMD/CLK. */
    masked(GRF_BASE, 0x238, 0xff00, 0x0700); /* Drive level 2 = 0x07. */
    masked(GRF_BASE, 0x23c, 0xffff, 0x0707);
    masked(GRF_BASE, 0x240, 0xffff, 0x0707);
    masked(GRF_BASE, 0x244, 0x00ff, 0x0007);
    masked(PMUGRF_BASE, 0x004, 0x000f, 0x0001); /* GPIO0 A4: SDMMC0_DET. */
    masked(PMUGRF_BASE, 0x020, 0x0300, 0x0100);

    /* Gate CIU while changing its mux to the crystal. RK3288-family MMC
     * integration divides the selected 24 MHz CIU source by two internally. */
    masked(CRU_BASE, 0x33c, 0x0002, 0x0002); /* CLK_GATE15: CIU gated. */
    masked(CRU_BASE, 0x178, 0x0700, 0x0000); /* CLK_SEL30: xin24m. */
    masked(CRU_BASE, 0x338, 0x0200, 0x0000); /* CLK_GATE14: HCLK_PHP. */
    masked(CRU_BASE, 0x33c, 0x0003, 0x0000); /* HCLK_SDMMC0 + CLK_SDMMC0. */
    masked(CRU_BASE, 0x434, 0x0018, 0x0018); /* reset IDs 211/212 */
    udelay(2);
    masked(CRU_BASE, 0x434, 0x0018, 0x0000);
    /* 90 degree drive, zero sample phase; no tuning at 12 MHz SDR. */
    masked(CRU_BASE, 0x580, 0x07fe, 0x0002);
    masked(CRU_BASE, 0x584, 0x07fe, 0x0000);
    static const dw_sdio_platform_data_t host = {
        .input_hz = 12000000,
        .max_hz = 12000000,
        .fifo_depth = 256,
    };
    device->plat_data = (void *)&host;
    device_put(device);
}
