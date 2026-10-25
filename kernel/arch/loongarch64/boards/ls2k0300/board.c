#ifdef CONFIG_BOARD_LS2K0300

#include "core/arch.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "platform.h"
#include "liointc.h"
#include "../../../common/module_devices.h"
#include "drivers/gpu/lsdc_boot.h"

/* The vendor bootloader owns DDR, UART clocks, pinmux and the baud divisor. */

uint64_t ls2k0300_timer_freq(void)
{
    uint32_t base = arch_read_cpu_config(4);
    uint32_t ratio = arch_read_cpu_config(5);
    uint32_t mult = ratio & 0xffffU;
    uint32_t div = ratio >> 16;
    /* Bad CPUCFG is a hard stop; inventing a frequency corrupts deadlines. */
    if (!base || !mult || !div) {
        arch_local_irq_disable();
        for (;;) arch_cpu_relax();
    }
    return (uint64_t)base * mult / div;
}

static uint64_t read_ticks(void) { return timer_get_ticks(); }
static const timer_ops_t stable_timer = {
    .read_ticks = read_ticks, .ticks_per_sec = ls2k0300_timer_freq,
};

static void early_init(void)
{
    timer_disable();
    ls2k0300_irq_init();
    loongarch64_memory_init();
    printf("[LS2K0300] stage=irq cpu=LA264 timer_hz=%lu CPUCFG.2=0x%x\n",
           (unsigned long)ls2k0300_timer_freq(), arch_read_cpu_config(2));
}

static void halt_board(void) { arch_local_irq_disable(); arch_halt(); }
static void prepare_scheduler(void)
{
#ifdef CONFIG_COOPERATIVE_BOOT
    timer_disable();
    printf("[LS2K0300] cooperative polling selected\n");
#else
    timer_enable();
    printf("[LS2K0300] LIOINTC UART RX and core timer enabled\n");
#endif
}

/* Only the documented low-DDR tail outside A20's allocator is eligible.
 * The reference firmware low bank ends at 240 MiB. Leave its upper 16 MiB
 * and the entire [2,128) MiB allocator untouched. No panel is configured. */
static const char display_compatible[] = "a20,ls2k0300-boot-display";
_Static_assert(PHYS_MEMORY_END <= 0x08000000,
               "allocator must not overlap retained scanout aperture");
static const fw_static_property_t display_properties[] = {
    {"compatible", {display_compatible, sizeof(display_compatible), FW_ORDER_NATIVE}},
};
static const fw_resource_t display_resources[] = {
    {FW_RESOURCE_MEMORY, 0x16090000, 0x2000, IORESOURCE_MMIO_32BIT, "registers"},
};
static const fw_static_node_t display_nodes[] = {
    {.name="ls2k0300-static", .parent=SIZE_MAX},
    {.name="display", .parent=0, .properties=display_properties, .property_count=1,
     .resources=display_resources, .resource_count=1},
};
static const fw_static_tree_t display_tree = {display_nodes, 2};
int ls2k0300_scanout_prepare(uint64_t physical, size_t bytes);
static const lsdc_boot_policy_t display_policy = {
    .magic=LSDC_BOOT_POLICY_MAGIC, .version=LSDC_BOOT_POLICY_VERSION,
    .register_base=0x16090000, .aperture_base=0x08000000,
    .aperture_bytes=0x07000000,
    .prepare=ls2k0300_scanout_prepare,
};
static void configure_devices(void)
{
    device_t *dev = device_find_get_by_mmio(0x16090000);
    if (!dev) {
        board_firmware_static_enumerate(&display_tree);
        dev = device_find_get_by_mmio(0x16090000);
        if (!dev) return;
    }
    resource_t *reg = device_get_resource(dev, RES_MMIO, 0);
    /* Only the known physical controller and exact 2K0300 identity receive
     * this architecture-owned DDR policy. Do not overwrite foreign data. */
    if (reg && reg->start == display_policy.register_base &&
        reg->end >= reg->start &&
        reg->end - reg->start >= LSDC_BOOT_REG_BYTES - 1 &&
        (fw_node_match_id(&dev->firmware_node, "loongson,la2k0300-dc") == 1 ||
         fw_node_match_id(&dev->firmware_node, "a20,ls2k0300-boot-display") == 1) &&
        !dev->plat_data)
        dev->plat_data = (void *)&display_policy;
    device_put(dev);
}

static const board_config_t ls2k0300 = {
    .name = "ls2k0300",
    .ram_base = PHYS_MEMORY_BASE, .ram_end = PHYS_MEMORY_END,
    .irqchip = &ls2k0300_irqchip_ops, .timer = &stable_timer,
    .early_init = early_init, .poweroff = halt_board, .reboot = halt_board,
    .prepare_scheduler = prepare_scheduler,
    .defer_network_init = 1,
    .configure_devices = configure_devices,
#ifdef CONFIG_COOPERATIVE_BOOT
    .idle_cannot_sleep = 1, .uart_rx_is_polled = 1,
#endif
};
const board_config_t *const current_board = &ls2k0300;
#endif
