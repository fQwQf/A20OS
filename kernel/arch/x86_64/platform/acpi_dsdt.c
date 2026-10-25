#include "core/errno.h"
#ifdef CONFIG_X86_64
/* Architecture contributes PCI bridge topology and standard pin swizzling.
 * AML execution and _PRT/link resource decoding use the shared uACPI service. */
#include "firmware.h"
#include "core/arch.h"
#include "drivers/bus/pci_hal.h"

typedef struct { unsigned bus, dev, function; } parent_bridge_t;
static int parent_bridge(unsigned bus, parent_bridge_t *out) {
    int found = 0;
    for (unsigned parent = 0; parent < 256; parent++) {
        for (unsigned dev = 0; dev < 32; dev++) {
            for (unsigned fn = 0; fn < 8; fn++) {
                uint32_t id = arch_pci_config_read32(parent, dev, fn, 0);
                if ((id & 0xffffU) == 0xffffU || !(id & 0xffffU)) continue;
                uint32_t header = arch_pci_config_read32(parent, dev, fn, 0xc);
                if (((header >> 16) & 0x7fU) != 1) continue;
                uint32_t buses = arch_pci_config_read32(parent, dev, fn, 0x18);
                unsigned primary = buses & 0xffU, secondary = (buses >> 8) & 0xffU;
                if (primary != parent || secondary != bus || primary == secondary) continue;
                if (found++) return -1; /* Ambiguous live topology is not a route. */
                *out = (parent_bridge_t){parent, dev, fn};
            }
        }
    }
    return found ? 0 : -1;
}
static int scope_for_bus(unsigned bus, unsigned depth, uint8_t visiting[256], fw_node_t *out) {
    if (bus > 255 || depth > 16 || visiting[bus]) return -1;
    if (!acpi_pci_root(0, bus, out)) return 0;
    visiting[bus] = 1;
    parent_bridge_t bridge;
    fw_node_t parent;
    int rc = parent_bridge(bus, &bridge) ||
             scope_for_bus(bridge.bus, depth + 1, visiting, &parent) ||
             acpi_pci_child(&parent, bridge.dev, bridge.function, out);
    visiting[bus] = 0;
    return rc ? -1 : 0;
}
uint32_t arch_pci_prt_gsi_fn(int bus, int dev, int function, int pin) {
    if (bus < 0 || bus > 255 || dev < 0 || dev > 31 || function < 0 || function > 7 ||
        pin < 1 || pin > 4) return PCI_PRT_GSI_NONE;
    fw_node_t root;
    if (acpi_namespace_root(&root)) {
        const acpi_context_t *context = firmware_acpi_context();
        if (!context || acpi_namespace_initialize(context, &root)) return PCI_PRT_GSI_NONE;
    }
    uint8_t climbed[256] = {0};
    for (unsigned depth = 0; depth < 16 && !climbed[bus]; depth++) {
        climbed[bus] = 1;
        uint8_t visiting[256] = {0};
        fw_node_t scope;
        uint32_t gsi;
        if (!scope_for_bus(bus, 0, visiting, &scope)) {
            int rc = acpi_pci_route(&scope, dev, function, pin, &gsi);
            if (!rc) return gsi;
            if (rc != -ENOENT) return PCI_PRT_GSI_NONE;
        }
        parent_bridge_t bridge;
        if (parent_bridge(bus, &bridge)) break;
        pin = ((pin - 1 + dev) & 3) + 1;
        bus = bridge.bus; dev = bridge.dev; function = bridge.function;
    }
    return PCI_PRT_GSI_NONE;
}
uint32_t arch_pci_prt_gsi(int bus, int dev, int pin) {
    return arch_pci_prt_gsi_fn(bus, dev, 0, pin);
}
#endif
