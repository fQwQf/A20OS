#ifndef A20_DRIVER_REGISTRY_TEST_H
#define A20_DRIVER_REGISTRY_TEST_H

#include "core/types.h"

enum driver_registry_test_kind {
    DRIVER_REGISTRY_TEST_DRIVER = 1,
    DRIVER_REGISTRY_TEST_DEVICE,
    DRIVER_REGISTRY_TEST_BUS,
};

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
/* Fail the next backing-array growth for this registry.  Test-only; the
 * production registration paths and allocator remain unchanged. */
void driver_registry_test_fail_next(int kind);
int driver_registry_test_last_failure(void);
void driver_registry_test_snapshot(int kind, int *count, int *capacity);
int driver_registry_capacity_test_run(void);
#endif

#endif
