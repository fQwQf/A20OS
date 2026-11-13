#include "firmware/irq.h"
#include "core/errno.h"
#include "core/string.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "mm/mm.h"

static fw_irq_domain_t *domains;
int fw_irq_domain_register(fw_irq_domain_t *d) {
    if (!d || !fw_node_valid(&d->controller) || !d->decode || !d->count ||
        d->logical_base >= PLATFORM_IRQ_MAX_LINES ||
        d->count > PLATFORM_IRQ_MAX_LINES - d->logical_base ||
        d->hardware_base > (uint32_t)-1 - (d->count - 1))
        return -EINVAL;
    for (fw_irq_domain_t *p = domains; p; p = p->next) {
        if (p == d || fw_node_equal(&p->controller, &d->controller))
            return -EEXIST;
        if (p->logical_base < d->logical_base + d->count &&
            d->logical_base < p->logical_base + p->count)
            return -EBUSY;
    }
    d->next = domains;
    domains = d;
    return 0;
}
int fw_irq_domain_unregister(fw_irq_domain_t *d) {
    for (fw_irq_domain_t **p = &domains; *p; p = &(*p)->next)
        if (*p == d) {
            *p = d->next;
            d->next = NULL;
            return 0;
        }
    return -ENOENT;
}
int fw_irq_from_hardware(const fw_irq_domain_t *d, uint32_t hwirq) {
    if (!d || hwirq < d->hardware_base || hwirq - d->hardware_base >= d->count)
        return -ERANGE;
    return (int)(d->logical_base + hwirq - d->hardware_base);
}
int fw_irq_resolve(const fw_reference_t *r, uint32_t *irq, uint32_t *flags) {
    if (!r || !irq || !flags || !fw_node_valid(&r->node))
        return -EINVAL;
    for (fw_irq_domain_t *p = domains; p; p = p->next) {
        if (!fw_node_equal(&p->controller, &r->node))
            continue;
        uint32_t hardware;
        int rc = p->decode(r, &hardware, flags);
        if (rc)
            return rc;
        uint32_t type = *flags;
        rc =
            p->map ? p->map(hardware, type) : fw_irq_from_hardware(p, hardware);
        if (rc < 0)
            return rc;
        if ((uint32_t)rc >= PLATFORM_IRQ_MAX_LINES ||
            (uint32_t)rc < p->logical_base ||
            (uint32_t)rc - p->logical_base >= p->count)
            return -ERANGE;
        *irq = (uint32_t)rc;
        if (type == 1 || type == 2)
            *flags = IORESOURCE_IRQ_EDGE;
        else if (type == 4 || type == 8)
            *flags = IORESOURCE_IRQ_LEVEL;
        else
            return -EINVAL;
        return 0;
    }
    return -EPROBE_DEFER;
}
static int trigger(uint32_t type, uint32_t *flags) {
    if (type != 1 && type != 2 && type != 4 && type != 8)
        return -EINVAL;
    *flags = type;
    return 0;
}
static int gic_decode(const fw_reference_t *r, uint32_t *hardware,
                      uint32_t *flags) {
    if (r->count != 3 || r->args[0] > 1)
        return -EOPNOTSUPP;
    if ((r->args[0] == 0 && r->args[1] >= 988) ||
        (r->args[0] == 1 && r->args[1] >= 16))
        return -ERANGE;
    *hardware = r->args[1] + (r->args[0] ? 16 : 32);
    return trigger(r->args[2] & 15, flags);
}
static int plic_decode(const fw_reference_t *r, uint32_t *hardware,
                       uint32_t *flags) {
    if (r->count != 1 || !r->args[0])
        return -EINVAL;
    *hardware = r->args[0];
    *flags = 4;
    return 0;
}
static int gsi_decode(const fw_reference_t *r, uint32_t *hardware,
                      uint32_t *flags) {
    if (r->count != 2)
        return -EINVAL;
    *hardware = r->args[0];
    return trigger(r->args[1], flags);
}
static int primary_kind(const fw_node_t *n) {
    if (fw_node_match_id(n, "arm,gic-v3") ||
        fw_node_match_id(n, "arm,gic-400") ||
        fw_node_match_id(n, "arm,cortex-a15-gic") ||
        fw_node_match_id(n, "arm,gic-v2"))
        return 1;
    if (fw_node_match_id(n, "riscv,plic0") ||
        fw_node_match_id(n, "sifive,plic-1.0.0") ||
        fw_node_match_id(n, "thead,c900-plic"))
        return 2;
    return 0;
}
static int find_primary(const fw_node_t *node, unsigned depth, fw_node_t *found,
                        int *kind, unsigned *count) {
    if (depth > 64)
        return -ELOOP;
    int rc = fw_node_available(node);
    if (rc <= 0)
        return rc;
    int type = primary_kind(node);
    if (type) {
        *found = *node;
        *kind = type;
        (*count)++;
    }
    fw_node_t child, previous;
    int has_previous = 0;
    while ((rc = fw_node_next_child(node, has_previous ? &previous : NULL,
                                    &child)) == 0) {
        previous = child;
        has_previous = 1;
        rc = find_primary(&child, depth + 1, found, kind, count);
        if (rc)
            return rc;
    }
    return rc == -ENOENT ? 0 : rc;
}
int firmware_irq_domains_prepare(const fw_node_t *root,
                                 fw_irq_domain_t **created) {
    if (!fw_node_valid(root) || !created)
        return -EINVAL;
    *created = NULL;
    int kind = 0, rc;
    unsigned count = 0;
    fw_node_t controller;
    const char *source = fw_node_source(root);
    if (source && !strcmp(source, "acpi")) {
        controller = *root;
        kind = 3;
        count = 1;
    } else {
        rc = find_primary(root, 0, &controller, &kind, &count);
        if (rc)
            return rc;
    }
    if (!count)
        return 0;
    if (count != 1)
        return -EOPNOTSUPP;
    for (fw_irq_domain_t *p = domains; p; p = p->next)
        if (fw_node_equal(&p->controller, &controller))
            return 0;
    fw_irq_domain_t *primary = kmalloc(sizeof(*primary));
    if (!primary)
        return -ENOMEM;
    *primary = (fw_irq_domain_t){.controller = controller,
                                 .count = PLATFORM_IRQ_MAX_LINES,
                                 .decode = kind == 1   ? gic_decode
                                           : kind == 2 ? plic_decode
                                                       : gsi_decode};
#ifdef CONFIG_ACPI
    if (kind == 3)
        primary->map = arch_firmware_irq_map;
#endif
    rc = fw_irq_domain_register(primary);
    if (rc)
        kfree(primary);
    else
        *created = primary;
    return rc;
}
int firmware_irq_domains_init(const fw_node_t *root) {
    fw_irq_domain_t *created;
    return firmware_irq_domains_prepare(root, &created);
}
void firmware_irq_domains_abort(fw_irq_domain_t *created) {
    if (created && !fw_irq_domain_unregister(created))
        kfree(created);
}

__attribute__((weak)) int firmware_legacy_irq_map(uint32_t hardware, uint32_t preferred)
{
    (void)hardware;
    if (preferred >= PLATFORM_IRQ_MAX_LINES)
        return -EINVAL;
    /* Only an architecture with a legacy ISA routing implementation can
     * supply this contract; arbitrary logical IRQ numbers are not a route. */
    return -EOPNOTSUPP;
}
