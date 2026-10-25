#ifndef A20_FIRMWARE_NODE_H
#define A20_FIRMWARE_NODE_H

#include "core/types.h"

/* A borrowed handle into an immutable, caller-owned firmware description.
 * Handles remain valid until every device referencing that description is gone.
 * No CPU virtual address or architecture convention is encoded in this API. */
typedef struct fw_node fw_node_t;
typedef struct fw_node_ops fw_node_ops_t;
struct fw_node {
    const fw_node_ops_t *ops;
    const void *context;
    uint64_t id;
};

enum fw_property_order {
    FW_ORDER_NATIVE,
    FW_ORDER_BIG_ENDIAN,
    FW_ORDER_LITTLE_ENDIAN
};
typedef struct fw_property {
    const void *data;
    size_t size;
    enum fw_property_order order;
} fw_property_t;

enum fw_resource_type { FW_RESOURCE_MEMORY, FW_RESOURCE_IO };
typedef struct fw_resource {
    enum fw_resource_type type;
    uint64_t address;
    uint64_t size;
    uint32_t flags;
    const char *name;
} fw_resource_t;

#define FW_REFERENCE_MAX_ARGS 16
/* A provider and its binding-defined arguments, e.g. clock ID or IRQ cells. */
typedef struct fw_reference {
    fw_node_t node;
    uint32_t args[FW_REFERENCE_MAX_ARGS];
    size_t count;
} fw_reference_t;

struct fw_node_ops {
    const char *source;
    const char *(*name)(const fw_node_t *node);
    int (*parent)(const fw_node_t *node, fw_node_t *out);
    /* previous == NULL starts iteration; -ENOENT is the end, other errors fail. */
    int (*next_child)(const fw_node_t *node, const fw_node_t *previous,
                      fw_node_t *out);
    int (*property)(const fw_node_t *node, const char *name,
                    fw_property_t *out);
    int (*reference)(const fw_node_t *node, const char *name,
                     const char *cells_name, size_t index, fw_reference_t *out);
    int (*resource)(const fw_node_t *node, size_t index, fw_resource_t *out);
    int (*irq)(const fw_node_t *node, size_t index, fw_reference_t *out);
    int (*available)(const fw_node_t *node);
    /* ACPI may expose functioning children beneath a non-present parent. */
    int (*children_available)(const fw_node_t *node);
    int (*match_id)(const fw_node_t *node, const char *id);
};

int fw_node_valid(const fw_node_t *node);
int fw_node_equal(const fw_node_t *a, const fw_node_t *b);
const char *fw_node_name(const fw_node_t *node);
const char *fw_node_source(const fw_node_t *node);
int fw_node_parent(const fw_node_t *node, fw_node_t *out);
int fw_node_next_child(const fw_node_t *node, const fw_node_t *previous,
                       fw_node_t *out);
int fw_node_property(const fw_node_t *node, const char *name,
                     fw_property_t *out);
int fw_node_has_property(const fw_node_t *node, const char *name);
int fw_node_read_u32_array(const fw_node_t *node, const char *name,
                           uint32_t *out, size_t count);
int fw_node_read_u64_array(const fw_node_t *node, const char *name,
                           uint64_t *out, size_t count);
int fw_node_read_u32(const fw_node_t *node, const char *name, uint32_t *out);
int fw_node_read_u64(const fw_node_t *node, const char *name, uint64_t *out);
int fw_node_read_string(const fw_node_t *node, const char *name, size_t index,
                        const char **out);
int fw_node_reference(const fw_node_t *node, const char *name,
                      const char *cells_name, size_t index,
                      fw_reference_t *out);
int fw_node_resource(const fw_node_t *node, size_t index, fw_resource_t *out);
int fw_node_irq(const fw_node_t *node, size_t index, fw_reference_t *out);
/* 1 = enabled, 0 = disabled, negative = invalid description. */
int fw_node_available(const fw_node_t *node);
int fw_node_children_available(const fw_node_t *node);
int fw_node_match_id(const fw_node_t *node, const char *id);

#endif
