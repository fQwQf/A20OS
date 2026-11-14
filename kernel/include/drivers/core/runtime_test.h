/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_RUNTIME_TEST_H
#define A20_DRIVER_RUNTIME_TEST_H
#ifndef CONFIG_MCU
/* Inert unless driver.runtime_test=1 is an exact boot argument. Called from
 * init_kthread after scheduler startup; success powers off the test machine. */
void driver_runtime_selftest_boot(void);
/* Explicit native IOMMU test, called only by the runtime boot gate. */
void driver_iommu_native_selftest(void);
#ifndef CONFIG_NOMMU
struct class_device;
/* Internal DRM fixture; explicit boot gate, embedded GPU code only. */
int drm_native_binding_selftest(struct class_device *publication);
#endif
#else
static inline void driver_runtime_selftest_boot(void) {}
#endif
#endif
