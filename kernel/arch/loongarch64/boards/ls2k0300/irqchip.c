#ifdef CONFIG_BOARD_LS2K0300
#include "core/arch.h"
#include "core/panic.h"
#include "core/errno.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "firmware/irq.h"
#include "boot/context.h"
#include "liointc.h"

static ls2k0300_lio_t controller;
static fw_irq_domain_t domains[2];
static uint32_t reg_read(void *context, unsigned offset) {
    (void)context;
    return *(volatile uint32_t *)(LS2K0300_UNCACHED_BASE +
                                  LS2K0300_LIOINTC_BASE + offset);
}
static void reg_write(void *context, unsigned offset, uint32_t value) {
    (void)context;
    *(volatile uint32_t *)(LS2K0300_UNCACHED_BASE + LS2K0300_LIOINTC_BASE +
                           offset) = value;
    arch_mb();
}
static void reg_write8(void *context, unsigned offset, uint8_t value) {
    (void)context;
    *(volatile uint8_t *)(LS2K0300_UNCACHED_BASE + LS2K0300_LIOINTC_BASE +
                          offset) = value;
    arch_mb();
}
static int decode(const fw_reference_t *r, uint32_t *hardware,
                  uint32_t *flags) {
    if (r->count != 2 || r->args[0] >= 32 ||
        (r->args[1] != 1 && r->args[1] != 2 && r->args[1] != 4 &&
         r->args[1] != 8))
        return -EINVAL;
    *hardware = r->args[0];
    *flags = r->args[1];
    return 0;
}
static int map(unsigned hardware, unsigned flags, unsigned bank) {
    if (hardware >= 32)
        return -ERANGE;
    unsigned irq = bank * 32 + hardware;
    /* Repeated resolution of a live shared line is allowed only when its
     * polarity/trigger matches; changing a live route is never implicit. */
    if (controller.enabled[bank] & (1U << hardware)) {
        unsigned polarity = !!(controller.polarity[bank] & (1U << hardware));
        unsigned edge = !!(controller.edge[bank] & (1U << hardware));
        unsigned current = edge ? (polarity ? 2 : 1) : (polarity ? 8 : 4);
        return current == flags ? (int)irq : -EBUSY;
    }
    int rc = ls2k0300_lio_configure(&controller, irq, flags);
    return rc ? rc : (int)irq;
}
static int map0(uint32_t irq, uint32_t flags) {
    return map(irq, flags, 0);
}
static int map1(uint32_t irq, uint32_t flags) {
    return map(irq, flags, 1);
}
static int register_domains(const fw_node_t *node, unsigned depth) {
    if (depth > 64)
        return -ELOOP;
    int rc = fw_node_available(node);
    if (rc <= 0)
        return rc;
    int matches = fw_node_match_id(node, "loongson,liointc-2.0");
    if (matches < 0)
        return matches;
    if (matches) {
        fw_resource_t resource;
        if (fw_node_resource(node, 0, &resource) ||
            resource.type != FW_RESOURCE_MEMORY || resource.size < 0x38)
            return -EINVAL;
        unsigned bank;
        if (resource.address == LS2K0300_LIOINTC_BASE)
            bank = 0;
        else if (resource.address == LS2K0300_LIOINTC_BASE + 0x40)
            bank = 1;
        else
            return -EINVAL;
        if (fw_node_valid(&domains[bank].controller))
            return -EEXIST;
        domains[bank] = (fw_irq_domain_t){.controller = *node,
                                          .logical_base = bank * 32,
                                          .count = 32,
                                          .decode = decode,
                                          .map = bank ? map1 : map0};
        if ((rc = fw_irq_domain_register(&domains[bank])))
            return rc;
    }
    fw_node_t child, previous;
    int have = 0;
    while (!(rc = fw_node_next_child(node, have ? &previous : NULL, &child))) {
        previous = child;
        have = 1;
        if ((rc = register_domains(&child, depth + 1)))
            return rc;
    }
    return rc == -ENOENT ? 0 : rc;
}
void ls2k0300_irq_init(void) {
    /* Disable the alternative extended route, preserving unrelated chip
     * configuration (manual 3.5: bit 19, both paths can otherwise assert). */
    volatile uint32_t *general =
        (void *)(LS2K0300_UNCACHED_BASE + 0x16000100UL);
    *general &= ~(1U << 19);
    arch_mb();
    const ls2k0300_lio_io_t io = {NULL, reg_read, reg_write, reg_write8};
    if (ls2k0300_lio_init(&controller, &io))
        panic("LS2K0300: interrupt controller initialization failed");
    const boot_context_t *boot = boot_context_get();
    if (fw_node_valid(&boot->hardware_root) &&
        register_domains(&boot->hardware_root, 0))
        panic("LS2K0300: invalid LIOINTC firmware description");
}
static void enable(uint32_t irq) {
    (void)ls2k0300_lio_enable(&controller, irq);
}
static void disable(uint32_t irq) {
    ls2k0300_lio_disable(&controller, irq);
}
const irqchip_ops_t ls2k0300_irqchip_ops = {
    .enable_irq = enable,
    .disable_irq = disable,
};
void ls2k0300_handle_device_irq(unsigned bank) {
    if (bank >= 2)
        return;
    uint32_t pending = *(volatile uint32_t *)(LS2K0300_UNCACHED_BASE +
                                              LS2K0300_LIOINTC_ISR0 + bank * 8);
    ls2k0300_lio_dispatch(&controller, bank, pending, driver_irq_dispatch);
}
#endif
