#ifndef A20_FIRMWARE_DEVICETREE_H
#define A20_FIRMWARE_DEVICETREE_H
#include "core/fdt_view.h"
#include "firmware/node.h"

typedef struct fw_dt {
    fdt_view_t view;
} fw_dt_t;
/* Context and blob remain caller-owned and immutable for all node lifetimes. */
/* The node provider returns translated CPU-physical reg resources. IRQs route
 * through interrupt-map nexuses to a controller, retaining its binding cells.
 * PCI unit-address cells are supported for IRQ matching; PCI MMIO decoding
 * remains the native PCI bus's responsibility. Unsupported pass-thru mappings
 * and dynamic firmware reservations fail explicitly. */
int fw_dt_init(fw_dt_t *tree, const void *blob, size_t available);
int fw_dt_root(const fw_dt_t *tree, fw_node_t *out);
int fw_dt_find_path(const fw_node_t *root, const char *path, fw_node_t *out);
int fw_dt_find_phandle(const fw_node_t *root, uint32_t handle, fw_node_t *out);
int fw_dt_bootargs(const fw_dt_t *tree, char *out, size_t capacity);

typedef struct fw_dt_range {
    uint64_t base, size;
} fw_dt_range_t;
typedef struct fw_dt_memory {
    fw_dt_range_t *ram, *reserved;
    size_t ram_capacity, reserved_capacity;
    size_t ram_count, reserved_count;
} fw_dt_memory_t;
/* No allocation; outputs sorted/merged full ranges. No missing-DTB fallback.
 * -ENOSPC: caller capacity insufficient, output counts reset (never truncated).
 * -EOPNOTSUPP: dynamic reserved-memory cannot be satisfied in this phase.
 * DTB's physical storage is reserved as part of the result. A valid tree with
 * no memory nodes succeeds with ram_count=0; reservations are still returned. */
int fw_dt_memory(const fw_dt_t *tree, uint64_t physical, fw_dt_memory_t *out);
/* Input arrays must be sorted and non-overlapping (as fw_dt_memory emits).
 * Preserves all segments on both sides of every reservation. */
int fw_dt_memory_subtract(const fw_dt_memory_t *map, fw_dt_range_t *out,
                          size_t capacity, size_t *count);
#define FW_DT_MEMORY_STATIC_IF_ABSENT 1U
/* Board policy intersection; preserves every usable segment and rejects any
 * firmware reservation overlapping the linked kernel. Page size is 4 KiB.
 * STATIC_IF_ABSENT permits a board-known RAM window only for a valid tree
 * lacking memory nodes, while still honoring its reservations and DTB. */
int fw_dt_memory_window(const fw_dt_t *tree, uint64_t physical,
                        uint64_t window_base, uint64_t window_end,
                        uint64_t kernel_base, uint64_t kernel_end,
                        fw_dt_range_t *out, size_t capacity, size_t *count,
                        unsigned flags);
#endif
