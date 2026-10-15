#include "proc/proc.h"

/* Compile this against the real kernel headers for each target ABI. The
 * exported symbol sizes make offsetof/sizeof/alignof visible in nm output. */
char eevdf_layout_size[sizeof(eevdf_node_t)];
char eevdf_layout_align[__alignof__(eevdf_node_t)];
char eevdf_layout_left[offsetof(eevdf_node_t, left) + 1];
char eevdf_layout_right[offsetof(eevdf_node_t, right) + 1];
char eevdf_layout_parent[offsetof(eevdf_node_t, parent) + 1];
char eevdf_layout_min[offsetof(eevdf_node_t, min_vruntime) + 1];
char eevdf_layout_heap[offsetof(eevdf_node_t, heap_prio) + 1];
char eevdf_layout_owner[offsetof(eevdf_node_t, owner) + 1];
char eevdf_layout_deadline[offsetof(eevdf_node_t, deadline_key) + 1];
char eevdf_layout_vruntime[offsetof(eevdf_node_t, vruntime_key) + 1];
