#ifndef A20_FIRMWARE_DEVICE_H
#define A20_FIRMWARE_DEVICE_H
#include "firmware/node.h"
struct device;
/* Materialize enabled, identifiable firmware devices independently of driver
 * availability. Re-enumeration is idempotent by firmware-node identity. */
int firmware_device_enumerate(const fw_node_t *root);
int firmware_device_prepare(struct device *device);
/* Returned pointers carry a reference released by device_put(). */
struct device *firmware_device_find(const fw_node_t *node);
int device_dependency_get(struct device *consumer, const char *property,
                          const char *cells_property, size_t index,
                          struct device **provider, fw_reference_t *reference);
#endif
