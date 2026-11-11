/* Driver/device/bus registry growth and failure-injection exercise. */
#include "drivers/core/driver_registry_test.h"
#include "drivers/core/driver_core.h"
#include "core/errno.h"
#include "core/klog.h"

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST

#define REGISTRY_TEST_LIMIT 256

static int registry_test_never_match(device_t *dev, const driver_t *drv)
{
    (void)dev;
    (void)drv;
    return 0;
}

static bus_type_t g_registry_test_device_bus = {
    .name = "registry-capacity-test-device-bus",
    .match = registry_test_never_match,
};

static int test_driver_registry(void)
{
    static driver_t entries[REGISTRY_TEST_LIMIT];
    int registered = 0;
    int failed = 0;
    int pass = 1;
    int base_count, base_capacity, count, capacity;

    driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DRIVER,
                                  &base_count, &base_capacity);

    driver_registry_test_fail_next(DRIVER_REGISTRY_TEST_DRIVER);
    for (int i = 0; i < REGISTRY_TEST_LIMIT; i++) {
        entries[i].name = "registry-capacity-test-driver";
        int ret = driver_register(&entries[i]);
        if (ret == -ENOMEM) {
            failed = 1;
            driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DRIVER,
                                          &count, &capacity);
            int old_duplicate = driver_register(&entries[0]);
            int retry = driver_register(&entries[i]);
            int new_duplicate = retry == 0 ? driver_register(&entries[i]) : 0;
            if (retry == 0)
                registered++;
            if (driver_registry_test_last_failure() != DRIVER_REGISTRY_TEST_DRIVER ||
                count != base_count + registered - (retry == 0) ||
                capacity != base_capacity || old_duplicate != -EEXIST ||
                retry != 0 || new_duplicate != -EEXIST) {
                kerr("[DRIVER-REGISTRY-TEST] driver old/fail/retry state mismatch\n");
                pass = 0;
            } else {
                driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DRIVER,
                                              &count, &capacity);
                if (count != base_count + registered ||
                    capacity != base_capacity * 2) {
                    kerr("[DRIVER-REGISTRY-TEST] driver retry capacity/count mismatch\n");
                    pass = 0;
                }
            }
            break;
        }
        if (ret != 0) {
            kerr("[DRIVER-REGISTRY-TEST] driver registration returned %d\n", ret);
            pass = 0;
            break;
        }
        registered++;
    }
    driver_registry_test_fail_next(0);
    if (!failed) {
        kerr("[DRIVER-REGISTRY-TEST] driver registry did not reach injected growth\n");
        pass = 0;
    }
    while (registered > 0) {
        int i = --registered;
        if (driver_unregister(&entries[i]) != 0) {
            kerr("[DRIVER-REGISTRY-TEST] driver cleanup failed at %d\n", i);
            pass = 0;
        }
    }
    driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DRIVER,
                                  &count, &capacity);
    if (count != base_count || capacity != base_capacity * 2) {
        kerr("[DRIVER-REGISTRY-TEST] driver cleanup did not restore count and expected backing capacity\n");
        pass = 0;
    }
    if (pass)
        kinfo("DRIVER_REGISTRY_CAPACITY kind=driver PASS old=retained fail=-ENOMEM retry=success count_restored=1 backing_capacity=%d\n",
              capacity);
    return pass ? 0 : -1;
}

static int test_device_registry(void)
{
    static device_t entries[REGISTRY_TEST_LIMIT];
    static const device_id_t sentinel_id = { .vendor = 1 };
    int registered = 0;
    int failed = 0;
    int pass = 1;
    int base_count, base_capacity, count, capacity;
    int drivers;

    driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DEVICE,
                                  &base_count, &base_capacity);
    driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DRIVER, &drivers, NULL);

    driver_registry_test_fail_next(DRIVER_REGISTRY_TEST_DEVICE);
    for (int i = 0; i < REGISTRY_TEST_LIMIT; i++) {
        entries[i].name = "registry-capacity-test-device";
        entries[i].bus = &g_registry_test_device_bus;
        entries[i].state = DEV_STATE_REMOVED;
        entries[i].matched_id = &sentinel_id;
        int ret = device_register(&entries[i]);
        if (ret == -ENOMEM) {
            failed = 1;
            driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DEVICE,
                                          &count, &capacity);
            int failed_device_unchanged =
                entries[i].state == DEV_STATE_REMOVED && !entries[i].drv &&
                entries[i].matched_id == &sentinel_id && !entries[i].class_dev;
            int old_duplicate = device_register(&entries[0]);
            int retry = device_register(&entries[i]);
            int new_duplicate = retry == 0 ? device_register(&entries[i]) : 0;
            if (retry == 0)
                registered++;
            if (driver_registry_test_last_failure() != DRIVER_REGISTRY_TEST_DEVICE ||
                count != base_count + registered - (retry == 0) ||
                capacity != base_capacity || old_duplicate != -EEXIST ||
                retry != 0 || new_duplicate != -EEXIST ||
                !failed_device_unchanged ||
                /* Successful registration attempts matching against existing
                 * drivers. An unmatched device must not keep a stale match;
                 * only the failed insertion above preserves the sentinel. */
                (retry == 0 && (entries[i].state != DEV_STATE_UNINIT ||
                                entries[i].matched_id != (drivers ? NULL : &sentinel_id) ||
                                entries[i].class_dev))) {
                kerr("[DRIVER-REGISTRY-TEST] device old/fail/retry state mismatch\n");
                pass = 0;
            } else {
                driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DEVICE,
                                              &count, &capacity);
                if (count != base_count + registered ||
                    capacity != base_capacity * 2) {
                    kerr("[DRIVER-REGISTRY-TEST] device retry capacity/count mismatch\n");
                    pass = 0;
                }
            }
            break;
        }
        if (ret != 0) {
            kerr("[DRIVER-REGISTRY-TEST] device registration returned %d\n", ret);
            pass = 0;
            break;
        }
        registered++;
    }
    driver_registry_test_fail_next(0);
    if (!failed) {
        kerr("[DRIVER-REGISTRY-TEST] device registry did not reach injected growth\n");
        pass = 0;
    }
    while (registered > 0)
        device_unregister(&entries[--registered]);
    driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_DEVICE,
                                  &count, &capacity);
    if (count != base_count || capacity != base_capacity * 2) {
        kerr("[DRIVER-REGISTRY-TEST] device cleanup did not restore count and expected backing capacity\n");
        pass = 0;
    }
    if (pass)
        kinfo("DRIVER_REGISTRY_CAPACITY kind=device PASS old=retained fail=-ENOMEM retry=success count_restored=1 backing_capacity=%d\n",
              capacity);
    return pass ? 0 : -1;
}

static int test_bus_registry(void)
{
    static bus_type_t entries[REGISTRY_TEST_LIMIT];
    int registered = 0;
    int failed = 0;
    int pass = 1;
    int base_count, base_capacity, count, capacity;

    driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_BUS,
                                  &base_count, &base_capacity);

    driver_registry_test_fail_next(DRIVER_REGISTRY_TEST_BUS);
    for (int i = 0; i < REGISTRY_TEST_LIMIT; i++) {
        entries[i].name = "registry-capacity-test-bus";
        int ret = bus_register(&entries[i]);
        if (ret == -ENOMEM) {
            failed = 1;
            driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_BUS,
                                          &count, &capacity);
            int old_duplicate = bus_register(&entries[0]);
            int retry = bus_register(&entries[i]);
            int new_duplicate = retry == 0 ? bus_register(&entries[i]) : 0;
            if (retry == 0)
                registered++;
            if (driver_registry_test_last_failure() != DRIVER_REGISTRY_TEST_BUS ||
                count != base_count + registered - (retry == 0) ||
                capacity != base_capacity || old_duplicate != -EEXIST ||
                retry != 0 || new_duplicate != -EEXIST) {
                kerr("[DRIVER-REGISTRY-TEST] bus old/fail/retry state mismatch\n");
                pass = 0;
            } else {
                driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_BUS,
                                              &count, &capacity);
                if (count != base_count + registered ||
                    capacity != base_capacity * 2) {
                    kerr("[DRIVER-REGISTRY-TEST] bus retry capacity/count mismatch\n");
                    pass = 0;
                }
            }
            break;
        }
        if (ret != 0) {
            kerr("[DRIVER-REGISTRY-TEST] bus registration returned %d\n", ret);
            pass = 0;
            break;
        }
        registered++;
    }
    driver_registry_test_fail_next(0);
    if (!failed) {
        kerr("[DRIVER-REGISTRY-TEST] bus registry did not reach injected growth\n");
        pass = 0;
    }
    while (registered > 0)
        bus_unregister(&entries[--registered]);
    driver_registry_test_snapshot(DRIVER_REGISTRY_TEST_BUS,
                                  &count, &capacity);
    if (count != base_count || capacity != base_capacity * 2) {
        kerr("[DRIVER-REGISTRY-TEST] bus cleanup did not restore count and expected backing capacity\n");
        pass = 0;
    }
    if (pass)
        kinfo("DRIVER_REGISTRY_CAPACITY kind=bus PASS old=retained fail=-ENOMEM retry=success count_restored=1 backing_capacity=%d\n",
              capacity);
    return pass ? 0 : -1;
}

int driver_registry_capacity_test_run(void)
{
    if (test_driver_registry() != 0 ||
        test_device_registry() != 0 ||
        test_bus_registry() != 0) {
        kerr("DRIVER_REGISTRY_CAPACITY: FAIL\n");
        return -1;
    }
    kinfo("DRIVER_REGISTRY_CAPACITY: PASS\n");
    return 0;
}

#endif /* CONFIG_DRIVER_LIFECYCLE_TEST */
