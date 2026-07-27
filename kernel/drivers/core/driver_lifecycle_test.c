/*
 * A20OS synthetic driver lifecycle test.
 *
 * Implements the Wave 2 P1 exercise: register a driver, register matching and
 * non-matching devices, force a probe failure, then success, remove, re-probe,
 * and unregister. The test is compile-time disabled by default under
 * CONFIG_DRIVER_LIFECYCLE_TEST.
 */
#include "drivers/core/driver_lifecycle_test.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST

#define LIFECYCLE_DRV_NAME "lifecycle-test"

typedef struct {
    int should_match;
    int should_driver_match;
    int fail_probe;
    int probe_count;
    int remove_count;
} lifecycle_plat_t;

static int lifecycle_match(device_t *dev, const driver_t *drv)
{
    if (!dev || !drv)
        return 0;
    if (strcmp(drv->name, LIFECYCLE_DRV_NAME) != 0)
        return 0;
    lifecycle_plat_t *plat = (lifecycle_plat_t *)dev->plat_data;
    return plat && plat->should_match;
}

static int lifecycle_probe(device_t *dev)
{
    lifecycle_plat_t *plat = (lifecycle_plat_t *)dev->plat_data;
    if (!plat)
        return -1;

    plat->probe_count++;
    if (plat->fail_probe) {
        plat->fail_probe = 0;
        kinfo("[DRIVER-LIFECYCLE] probe failure forced for '%s'\n",
              dev->name ? dev->name : "?");
        return -EIO;
    }

    dev->drv_priv = plat;
    kinfo("[DRIVER-LIFECYCLE] probe succeeded for '%s'\n",
          dev->name ? dev->name : "?");
    return 0;
}

static int lifecycle_driver_match(device_t *dev)
{
    lifecycle_plat_t *plat = dev ? (lifecycle_plat_t *)dev->plat_data : NULL;
    return plat && plat->should_driver_match;
}

static int lifecycle_remove(device_t *dev)
{
    lifecycle_plat_t *plat = (lifecycle_plat_t *)dev->drv_priv;
    if (plat)
        plat->remove_count++;
    dev->drv_priv = NULL;
    kinfo("[DRIVER-LIFECYCLE] remove called for '%s'\n",
          dev->name ? dev->name : "?");
    return 0;
}

static int lifecycle_char_read(device_t *dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    return (int)count;
}

static const char_dev_ops_t lifecycle_char_ops = {
    .read = lifecycle_char_read,
};

static bus_type_t g_lifecycle_bus = {
    .name  = "lifecycle",
    .match = lifecycle_match,
    .probe = NULL,
    .remove = NULL,
};

static const device_id_t g_lifecycle_ids[] = {
    { .vendor = 0xA20A, .device = 0x0001 },
    { 0 },
};

static driver_t g_lifecycle_driver = {
    .name       = LIFECYCLE_DRV_NAME,
    .id_table   = g_lifecycle_ids,
    .bus        = &g_lifecycle_bus,
    .match      = lifecycle_driver_match,
    .probe      = lifecycle_probe,
    .remove     = lifecycle_remove,
    .class_ops  = &lifecycle_char_ops,
    .class_type = DEV_CLASS_CHAR,
};

int driver_lifecycle_test_run(void)
{
    int pass = 1;

    lifecycle_plat_t plat_nomatch = { .should_match = 0 };
    lifecycle_plat_t plat_driver_nomatch = { .should_match = 1 };
    lifecycle_plat_t plat_fail = {
        .should_match = 1, .should_driver_match = 1, .fail_probe = 1,
    };
    lifecycle_plat_t plat_ok = {
        .should_match = 1, .should_driver_match = 1,
    };

    device_t dev_nomatch = {
        .name      = "lifecycle-nomatch",
        .bus       = &g_lifecycle_bus,
        .plat_data = &plat_nomatch,
    };

    device_t dev_fail = {
        .name      = "lifecycle-fail",
        .bus       = &g_lifecycle_bus,
        .plat_data = &plat_fail,
    };

    device_t dev_driver_nomatch = {
        .name      = "lifecycle-driver-nomatch",
        .bus       = &g_lifecycle_bus,
        .plat_data = &plat_driver_nomatch,
    };

    device_t dev_ok = {
        .name      = "lifecycle-ok",
        .bus       = &g_lifecycle_bus,
        .plat_data = &plat_ok,
    };

    kinfo("[DRIVER-LIFECYCLE] starting synthetic lifecycle exercise\n");

    /* 1. Register the synthetic bus. */
    if (bus_register(&g_lifecycle_bus) != 0) {
        kerr("[DRIVER-LIFECYCLE] bus_register failed\n");
        pass = 0;
        goto out;
    }
    if (bus_register(&g_lifecycle_bus) != -EEXIST) {
        kerr("[DRIVER-LIFECYCLE] duplicate bus registration was accepted\n");
        pass = 0;
        goto out;
    }

    /* 2. Register the synthetic driver. */
    if (driver_register(&g_lifecycle_driver) != 0) {
        kerr("[DRIVER-LIFECYCLE] driver_register failed\n");
        pass = 0;
        goto out;
    }
    if (driver_register(&g_lifecycle_driver) != -EEXIST) {
        kerr("[DRIVER-LIFECYCLE] duplicate driver registration was accepted\n");
        pass = 0;
        goto out;
    }

    /* 3. Register a non-matching device; it must stay unbound. */
    if (device_register(&dev_nomatch) != 0) {
        kerr("[DRIVER-LIFECYCLE] device_register(nomatch) failed\n");
        pass = 0;
        goto out;
    }
    if (device_register(&dev_nomatch) != -EEXIST) {
        kerr("[DRIVER-LIFECYCLE] duplicate device registration was accepted\n");
        pass = 0;
        goto out;
    }
    if (dev_nomatch.drv != NULL || dev_nomatch.state != DEV_STATE_UNINIT) {
        kerr("[DRIVER-LIFECYCLE] non-matching device became bound unexpectedly\n");
        pass = 0;
        goto out;
    }

    /* 4. A bus match rejected by the driver's protocol match stays unbound. */
    if (device_register(&dev_driver_nomatch) != 0 ||
        dev_driver_nomatch.drv != NULL ||
        dev_driver_nomatch.state != DEV_STATE_UNINIT ||
        dev_driver_nomatch.matched_id != NULL ||
        plat_driver_nomatch.probe_count != 0) {
        kerr("[DRIVER-LIFECYCLE] driver match rejection was ignored\n");
        pass = 0;
        goto out;
    }

    /* 5. Register a matching device whose probe is forced to fail. */
    if (device_register(&dev_fail) != 0) {
        kerr("[DRIVER-LIFECYCLE] device_register(fail) failed\n");
        pass = 0;
        goto out;
    }
    if (dev_fail.drv != NULL || dev_fail.state != DEV_STATE_UNINIT) {
        kerr("[DRIVER-LIFECYCLE] failing probe left a half-bound device\n");
        pass = 0;
        goto out;
    }
    if (plat_fail.probe_count != 1) {
        kerr("[DRIVER-LIFECYCLE] forced probe was not attempted\n");
        pass = 0;
        goto out;
    }
    device_unregister(&dev_fail);

    /* 6. Register a matching device whose probe succeeds. */
    if (device_register(&dev_ok) != 0) {
        kerr("[DRIVER-LIFECYCLE] device_register(ok) failed\n");
        pass = 0;
        goto out;
    }
    if (dev_ok.drv != &g_lifecycle_driver || dev_ok.state != DEV_STATE_PROBED) {
        kerr("[DRIVER-LIFECYCLE] successful probe did not bind device\n");
        pass = 0;
        goto out;
    }
    if (plat_ok.probe_count != 1) {
        kerr("[DRIVER-LIFECYCLE] successful probe was not attempted\n");
        pass = 0;
        goto out;
    }
    if (!dev_ok.class_dev || !dev_ok.class_dev->online) {
        kerr("[DRIVER-LIFECYCLE] successful probe was not published\n");
        pass = 0;
        goto out;
    }
    class_device_t *stale = dev_ok.class_dev;
    class_device_get(stale);

    /* 7. Unregister the driver; remove must be called on bound devices. */
    if (driver_unregister(&g_lifecycle_driver) != 0) {
        kerr("[DRIVER-LIFECYCLE] driver_unregister failed\n");
        pass = 0;
        goto out;
    }
    if (dev_ok.drv != NULL || dev_ok.state != DEV_STATE_REMOVED) {
        kerr("[DRIVER-LIFECYCLE] driver_unregister did not unbind device\n");
        pass = 0;
        goto out;
    }
    if (plat_ok.remove_count != 1) {
        kerr("[DRIVER-LIFECYCLE] remove callback not invoked during unregister\n");
        pass = 0;
        goto out;
    }
    if (dev_ok.class_dev != NULL || class_device_call_begin(stale) != -ENODEV) {
        kerr("[DRIVER-LIFECYCLE] unbind left class publication online\n");
        class_device_put(stale);
        pass = 0;
        goto out;
    }
    class_device_put(stale);

    /* 8. Re-registering synchronously probes all existing unbound devices. */
    if (driver_register(&g_lifecycle_driver) != 0) {
        kerr("[DRIVER-LIFECYCLE] driver re-register failed\n");
        pass = 0;
        goto out;
    }
    if (dev_ok.drv != &g_lifecycle_driver || dev_ok.state != DEV_STATE_PROBED) {
        kerr("[DRIVER-LIFECYCLE] driver re-register did not bind device\n");
        pass = 0;
        goto out;
    }
    if (plat_ok.probe_count != 2) {
        kerr("[DRIVER-LIFECYCLE] automatic re-probe was not attempted once\n");
        pass = 0;
        goto out;
    }
    if (bus_probe_device(&dev_ok) != -EBUSY) {
        kerr("[DRIVER-LIFECYCLE] explicitly re-probed an already bound device\n");
        pass = 0;
        goto out;
    }

    /* 9. Remove the device explicitly. */
    device_unregister(&dev_ok);
    if (dev_ok.drv != NULL || dev_ok.state != DEV_STATE_REMOVED) {
        kerr("[DRIVER-LIFECYCLE] device_unregister did not unbind device\n");
        pass = 0;
        goto out;
    }
    if (plat_ok.remove_count != 2) {
        kerr("[DRIVER-LIFECYCLE] remove callback not invoked during device_unregister\n");
        pass = 0;
        goto out;
    }

out:
    /* These objects live on this stack frame.  Always remove them from the
     * global registries, including after an assertion failure, so later boot
     * code cannot observe dangling device or platform-data pointers. */
    device_unregister(&dev_ok);
    device_unregister(&dev_fail);
    device_unregister(&dev_driver_nomatch);
    device_unregister(&dev_nomatch);
    driver_unregister(&g_lifecycle_driver);
    bus_unregister(&g_lifecycle_bus);

    if (pass) {
        kinfo("DRIVER_LIFECYCLE: PASS\n");
        return 0;
    }
    kerr("DRIVER_LIFECYCLE: FAIL\n");
    return -1;
}

#endif /* CONFIG_DRIVER_LIFECYCLE_TEST */
