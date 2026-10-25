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
/* Firmware-retained consumers call this from probe. Referenced clocks, resets
 * and power domains remain in their boot state: provider first-probe is deferred
 * until all managed holds are released. A provider already bound to a native
 * driver requires an explicit handoff and is rejected. No DT properties change.
 * Arbitrary regulator *-supply properties are not managed by this API; a
 * future regulator framework must add explicit retention before takeover. */
int firmware_device_retain_resources(struct device *consumer);

int device_dependency_get(struct device *consumer, const char *property,
                          const char *cells_property, size_t index,
                          struct device **provider, fw_reference_t *reference);
#endif
