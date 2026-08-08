/*
 * A20OS Driver Core — registration, matching, enumeration
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_class.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/panic.h"
#include "core/errno.h"
#include "mm/slab.h"

/* DRIVER_CORE_DYNAMIC_LIMITS: initial capacity for bringup; registries grow
 * dynamically via krealloc when capacity is exhausted. */
#define DRIVER_INITIAL_CAP  32
#define DEVICE_INITIAL_CAP  64
#define BUS_INITIAL_CAP     8

/* DRIVER_CORE_CONCURRENCY_MODEL: registry arrays and count fields are protected
 * by driver_core_lock. Probe/remove callbacks run after binding decisions and
 * must leave dev->drv/dev->state consistent on failure. */
static spinlock_t g_driver_core_lock = SPINLOCK_INIT;
/* Serializes registry mutation with probe/remove callbacks.  The spinlock only
 * protects the arrays themselves and is never held across driver code. */
static mutex_t g_driver_core_ops = MUTEX_INIT;

static driver_t   **g_drivers;
static int         g_driver_count;
static int         g_driver_cap;
static device_t   **g_devices;
static int         g_device_count;
static int         g_device_cap;
static bus_type_t **g_buses;
static int         g_bus_count;
static int         g_bus_cap;

static int driver_matches_device(driver_t *drv, device_t *dev)
{
    int match = 0;
    if (dev->bus && dev->bus->match)
        match = dev->bus->match(dev, drv);
    else if (!dev->bus && !drv->bus)
        /* A busless device has no bus identity to match against; it binds
         * only when the driver explicitly accepts it via its match()
         * callback.  There is deliberately no wildcard: a busless driver
         * must never claim an unrelated board device by accident. */
        match = drv->match ? drv->match(dev) : 0;
    if (match && drv->match && !drv->match(dev)) {
        dev->matched_id = NULL;
        match = 0;
    }
    /* One owner per device: a user-owned device accepts only read-only
     * kernel probes; the owning user-service driver drives it. */
    if (match && dev->user_owned && !drv->read_only_probe) {
        dev->matched_id = NULL;
        match = 0;
    }
    return match;
}

/* ---- Linker-generated section boundaries for built-in drivers ---- */
extern const driver_t *__driver_init_start;
extern const driver_t *__driver_init_end;

static int driver_probe_bound_device(driver_t *drv, device_t *dev) {
    dev->drv = drv;
    if (!drv->probe) {
        dev->state = DEV_STATE_PROBED;
        return 0;
    }
    int ret = drv->probe(dev);
    if (ret == 0) {
        dev->state = DEV_STATE_PROBED;
        ret = class_device_publish(dev);
        if (ret < 0) {
            if (drv->remove)
                drv->remove(dev);
            dev->drv = NULL;
            dev->drv_priv = NULL;
            dev->matched_id = NULL;
            dev->state = DEV_STATE_UNINIT;
            return ret;
        }
        return 0;
    }
    /* DRIVER_PROBE_FAILURE_CLEANUP: failed probes leave no half-bound device. */
    dev->drv = NULL;
    dev->drv_priv = NULL;
    dev->matched_id = NULL;
    dev->state = DEV_STATE_UNINIT;
    return ret;
}

/* ============================================================
 * driver_core_init — called early from kernel_main
 *
 * Iterates .driver_init linker section and registers every
 * built-in driver.  Then probes all devices registered by
 * board_init().
 * ============================================================ */
void driver_core_init(void) {
    spin_init(&g_driver_core_lock);
    mutex_init(&g_driver_core_ops);
    g_driver_count = 0;
    g_driver_cap   = DRIVER_INITIAL_CAP;
    g_device_count = 0;
    g_device_cap   = DEVICE_INITIAL_CAP;
    g_bus_count    = 0;
    g_bus_cap      = BUS_INITIAL_CAP;

    g_drivers = kmalloc(sizeof(driver_t *) * g_driver_cap);
    g_devices = kmalloc(sizeof(device_t *) * g_device_cap);
    g_buses   = kmalloc(sizeof(bus_type_t *) * g_bus_cap);
    if (!g_drivers || !g_devices || !g_buses) {
        panic("driver_core_init: kmalloc failed\n");
    }

    for (const driver_t * const *p = (const driver_t * const *)&__driver_init_start;
         p < (const driver_t * const *)&__driver_init_end; p++) {
        driver_register((driver_t *)*p);
    }

    kinfo("[DRIVER] core initialized: %d drivers registered\n",
          g_driver_count);
}

/* ============================================================
 * driver_register — add a driver to the system
 *
 * After registration, scans existing unbound devices for matches.
 * ============================================================ */
int driver_register(driver_t *drv) {
    if (!drv || !drv->name)
        return -EINVAL;

    mutex_lock(&g_driver_core_ops);
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_driver_count; i++) {
        if (g_drivers[i] == drv) {
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            mutex_unlock(&g_driver_core_ops);
            return -EEXIST;
        }
    }
    if (g_driver_count >= g_driver_cap) {
        int new_cap = g_driver_cap * 2;
        driver_t **new_arr = krealloc(g_drivers, sizeof(driver_t *) * new_cap);
        if (!new_arr) {
            kerr("[DRIVER] driver_register: capacity exhausted (%d)\n", g_driver_cap);
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            mutex_unlock(&g_driver_core_ops);
            return -ENOMEM;
        }
        g_drivers = new_arr;
        g_driver_cap = new_cap;
    }

    g_drivers[g_driver_count++] = drv;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);

    kinfo("[DRIVER] registered driver '%s' (class=%d)\n",
          drv->name, drv->class_type);

    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv != NULL)
            continue;
        if (driver_matches_device(drv, dev)) {
            int ret = driver_probe_bound_device(drv, dev);
            if (ret == 0) {
                kinfo("[DRIVER] device '%s' bound to driver '%s'\n",
                      dev->name, drv->name);
            } else {
                kdebug("[DRIVER] probe '%s' -> '%s' failed: %d\n",
                       dev->name, drv->name, ret);
            }
        }
    }
    mutex_unlock(&g_driver_core_ops);
    return 0;
}

int driver_unregister(driver_t *drv) {
    if (!drv) return -EINVAL;
    mutex_lock(&g_driver_core_ops);
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_driver_count; i++) {
        if (g_drivers[i] == drv) {
            g_drivers[i] = g_drivers[--g_driver_count];
            spin_unlock_irqrestore(&g_driver_core_lock, flags);

            /* Lifecycle callbacks may release IRQs, DMA memory, or sleep. */
            for (int j = 0; j < g_device_count; j++) {
                device_t *dev = g_devices[j];
                if (dev->drv != drv)
                    continue;
                dev->state = DEV_STATE_REMOVING;
                class_device_unpublish(dev);
                if (drv->remove)
                    drv->remove(dev);
                dev->drv = NULL;
                dev->drv_priv = NULL;
                dev->matched_id = NULL;
                dev->state = DEV_STATE_REMOVED;
            }
            mutex_unlock(&g_driver_core_ops);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    mutex_unlock(&g_driver_core_ops);
    return -ENOENT;
}

/* ============================================================
 * device_register — add a device to the system
 *
 * After registration, scans all registered drivers for a match.
 * ============================================================ */
int device_register(device_t *dev) {
    if (!dev || !dev->name)
        return -EINVAL;

    mutex_lock(&g_driver_core_ops);
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i] == dev) {
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            mutex_unlock(&g_driver_core_ops);
            return -EEXIST;
        }
    }
    if (g_device_count >= g_device_cap) {
        int new_cap = g_device_cap * 2;
        device_t **new_arr = krealloc(g_devices, sizeof(device_t *) * new_cap);
        if (!new_arr) {
            kerr("[DRIVER] device_register: capacity exhausted (%d)\n", g_device_cap);
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            mutex_unlock(&g_driver_core_ops);
            return -ENOMEM;
        }
        g_devices = new_arr;
        g_device_cap = new_cap;
    }
    g_devices[g_device_count++] = dev;
    dev->state = DEV_STATE_UNINIT;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);

    kinfo("[DRIVER] registered device '%s' (bus=%s)\n",
          dev->name ? dev->name : "?",
          (dev->bus && dev->bus->name) ? dev->bus->name : "?");

    for (int i = 0; i < g_driver_count; i++) {
        driver_t *drv = g_drivers[i];
        if (driver_matches_device(drv, dev)) {
            int ret = driver_probe_bound_device(drv, dev);
            if (ret == 0) {
                kinfo("[DRIVER] device '%s' bound to driver '%s'\n",
                      dev->name, drv->name);
                mutex_unlock(&g_driver_core_ops);
                return 0;
            }
        }
    }
    mutex_unlock(&g_driver_core_ops);
    return 0;
}

void device_unregister(device_t *dev) {
    if (!dev) return;
    mutex_lock(&g_driver_core_ops);
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i] == dev) {
            g_devices[i] = g_devices[--g_device_count];
            spin_unlock_irqrestore(&g_driver_core_lock, flags);

            driver_t *drv = dev->drv;
            dev->state = DEV_STATE_REMOVING;
            class_device_unpublish(dev);
            if (drv && drv->remove)
                drv->remove(dev);
            dev->drv = NULL;
            dev->drv_priv = NULL;
            dev->state = DEV_STATE_REMOVED;
            dev->matched_id = NULL;
            mutex_unlock(&g_driver_core_ops);
            return;
        }
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    mutex_unlock(&g_driver_core_ops);
}

int bus_register(bus_type_t *bus) {
    if (!bus || !bus->name)
        return -EINVAL;
    mutex_lock(&g_driver_core_ops);
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_bus_count; i++) {
        if (g_buses[i] == bus) {
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            mutex_unlock(&g_driver_core_ops);
            return -EEXIST;
        }
    }
    if (g_bus_count >= g_bus_cap) {
        int new_cap = g_bus_cap * 2;
        bus_type_t **new_arr = krealloc(g_buses, sizeof(bus_type_t *) * new_cap);
        if (!new_arr) {
            kerr("[DRIVER] bus_register: capacity exhausted (%d)\n", g_bus_cap);
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            mutex_unlock(&g_driver_core_ops);
            return -ENOMEM;
        }
        g_buses = new_arr;
        g_bus_cap = new_cap;
    }
    g_buses[g_bus_count++] = bus;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    mutex_unlock(&g_driver_core_ops);
    kinfo("[DRIVER] registered bus '%s'\n", bus->name);
    return 0;
}

void bus_unregister(bus_type_t *bus) {
    if (!bus) return;
    mutex_lock(&g_driver_core_ops);
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_bus_count; i++) {
        if (g_buses[i] == bus) {
            g_buses[i] = g_buses[--g_bus_count];
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            mutex_unlock(&g_driver_core_ops);
            return;
        }
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    mutex_unlock(&g_driver_core_ops);
}

int bus_probe_device(device_t *dev) {
    if (!dev) return -EINVAL;
    mutex_lock(&g_driver_core_ops);
    if (dev->drv) {
        mutex_unlock(&g_driver_core_ops);
        return -EBUSY;
    }
    for (int i = 0; i < g_driver_count; i++) {
        driver_t *drv = g_drivers[i];
        if (driver_matches_device(drv, dev)) {
            if (driver_probe_bound_device(drv, dev) == 0) {
                mutex_unlock(&g_driver_core_ops);
                return 0;
            }
        }
    }
    mutex_unlock(&g_driver_core_ops);
    return -ENODEV;
}

resource_t *device_get_resource(device_t *dev, enum resource_type type, int index) {
    if (!dev) return NULL;
    int found = 0;
    for (int i = 0; i < dev->res_count; i++) {
        if (dev->res[i].type == type) {
            if (found == index)
                return &dev->res[i];
            found++;
        }
    }
    return NULL;
}

device_t *device_find_by_class(uint32_t class_type, int index) {
    int found = 0;
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv && dev->drv->class_type == class_type) {
            if (found == index)
                return dev;
            found++;
        }
    }
    return NULL;
}

void driver_probe_all(void) {
    int probed = 0;
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv || dev->state >= DEV_STATE_PROBED)
            continue;
        if (bus_probe_device(dev) == 0)
            probed++;
    }
    kinfo("[DRIVER] probe_all: %d devices probed\n", probed);
}

void driver_progress_class(uint32_t class_type)
{
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv && dev->drv->class_type == class_type && dev->drv->progress)
            dev->drv->progress(dev);
    }
}
