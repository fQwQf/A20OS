#ifndef A20_FIRMWARE_IRQ_H
#define A20_FIRMWARE_IRQ_H
#include "firmware/node.h"

typedef struct fw_irq_domain fw_irq_domain_t;
struct fw_irq_domain {
    fw_node_t controller;
    uint32_t hardware_base, logical_base, count;
    int (*decode)(const fw_reference_t *, uint32_t *hardware, uint32_t *flags);
    /* Optional controller routing; decode flags use IRQ_TYPE values 1/2/4/8. */
    int (*map)(uint32_t hardware, uint32_t trigger_flags);
    fw_irq_domain_t *next;
};
/* Domains are provider-owned, registered before consumer probing, and must
 * outlive their consumers. Logical ranges must be disjoint. */
int fw_irq_domain_register(fw_irq_domain_t *domain);
int fw_irq_domain_unregister(fw_irq_domain_t *domain);
int fw_irq_resolve(const fw_reference_t *reference, uint32_t *irq,
                   uint32_t *flags);
int fw_irq_from_hardware(const fw_irq_domain_t *domain, uint32_t hardware);
/* Discover the primary GIC/PLIC or ACPI GSI domain. Multiple primary
 * controllers require explicit controller-driver domain registration. */
int firmware_irq_domains_init(const fw_node_t *root);
/* Enumeration transaction: created is NULL for an existing domain; a newly
 * allocated automatic domain must be aborted if device discovery fails. */
int firmware_irq_domains_prepare(const fw_node_t *root,
                                 fw_irq_domain_t **created);
void firmware_irq_domains_abort(fw_irq_domain_t *created);
/* ACPI GSI -> architecture driver IRQ id; flags use DT trigger encodings. */
int arch_firmware_irq_map(uint32_t hardware, uint32_t trigger_flags);
/* ISA controller fallback, supplied by the platform; callers do not program
 * IOAPIC/PIC routes themselves. Returns a driver IRQ ID or negative errno. */
int firmware_legacy_irq_map(uint32_t hardware, uint32_t preferred);
#endif
