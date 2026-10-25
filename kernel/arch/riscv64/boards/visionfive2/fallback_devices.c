#include "../../../common/module_devices.h"
#include "boot/context.h"

/* Used only without firmware. A real DT describes its own resources/IRQs. */
static const char plic_id[] = "riscv,plic0";
static const char sdio_id[] = "starfive,jh7110-mmc";
static const char gmac_id[] = "starfive,jh7110-dwmac";
static const uint32_t interrupt_cells = 1;
static const fw_static_property_t plic_properties[] = {
    {"compatible", {plic_id, sizeof(plic_id), FW_ORDER_NATIVE}},
    {"interrupt-controller", {NULL, 0, FW_ORDER_NATIVE}},
    {"#interrupt-cells",
     {&interrupt_cells, sizeof(interrupt_cells), FW_ORDER_NATIVE}},
};
static const fw_static_property_t sdio_properties[] = {
    {"compatible", {sdio_id, sizeof(sdio_id), FW_ORDER_NATIVE}},
};
static const fw_static_property_t gmac_properties[] = {
    {"compatible", {gmac_id, sizeof(gmac_id), FW_ORDER_NATIVE}},
};
static const fw_resource_t sdio_resources[] = {
    {FW_RESOURCE_MEMORY, 0x16020000, 0x10000, IORESOURCE_MMIO_32BIT, "mshc"},
};
static const fw_resource_t gmac_resources[] = {
    {FW_RESOURCE_MEMORY, 0x16040000, 0x10000, IORESOURCE_MMIO_32BIT, "gmac1"},
};
static const uint32_t gmac_interrupt[] = {78};
static const fw_static_reference_t gmac_irqs[] = {
    {.provider = 1, .args = gmac_interrupt, .count = 1},
};
static const fw_static_node_t nodes[] = {
    {.name = "visionfive2-static", .parent = SIZE_MAX},
    {.name = "plic",
     .parent = 0,
     .properties = plic_properties,
     .property_count = 3},
    /* The no-DT SDIO route is unknown; absence of an IRQ selects polling. */
    {.name = "dw-sdio0",
     .parent = 0,
     .properties = sdio_properties,
     .property_count = 1,
     .resources = sdio_resources,
     .resource_count = 1},
    {.name = "starfive-gmac",
     .parent = 0,
     .properties = gmac_properties,
     .property_count = 1,
     .resources = gmac_resources,
     .resource_count = 1,
     .irqs = gmac_irqs,
     .irq_count = 1},
};
static const fw_static_tree_t tree = {nodes, sizeof(nodes) / sizeof(nodes[0])};
void vf2_enumerate_static_devices(void) {
    const boot_context_t *boot = boot_context_get();
    if (boot && fw_node_valid(&boot->hardware_root))
        return;
    board_firmware_static_enumerate(&tree);
}
