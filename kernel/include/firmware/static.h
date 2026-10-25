#ifndef A20_FIRMWARE_STATIC_H
#define A20_FIRMWARE_STATIC_H
#include "firmware/node.h"
/* Compile-time descriptors use precisely the same node/property/resource API
 * as DT and ACPI. No fake DT blob or mandatory runtime allocation is needed. */
typedef struct fw_static_property {
    const char *name;
    fw_property_t value;
} fw_static_property_t;
typedef struct fw_static_reference {
    const char *property;
    size_t provider;
    const uint32_t *args;
    size_t count;
} fw_static_reference_t;
typedef struct fw_static_node {
    const char *name;
    size_t parent; /* SIZE_MAX denotes a root */
    const fw_static_property_t *properties;
    size_t property_count;
    const fw_resource_t *resources;
    size_t resource_count;
    const fw_static_reference_t *references;
    size_t reference_count;
    const fw_static_reference_t *irqs;
    size_t irq_count;
    int disabled;
} fw_static_node_t;
typedef struct fw_static_tree {
    const fw_static_node_t *nodes;
    size_t count;
} fw_static_tree_t;
int fw_static_init(const fw_static_tree_t *tree, fw_node_t *root);
int fw_static_get(const fw_static_tree_t *tree, size_t index, fw_node_t *node);
#endif
