/*
 * A20OS Driver Core — registration, matching, enumeration
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/defs.h"

#define MAX_DRIVERS  32
#define MAX_DEVICES  64
#define MAX_BUSES    8

static driver_t   *g_drivers[MAX_DRIVERS];
static int         g_driver_count;
static device_t   *g_devices[MAX_DEVICES];
static int         g_device_count;
static bus_type_t *g_buses[MAX_BUSES];
static int         g_bus_count;

/* ---- Linker-generated section boundaries for built-in drivers ---- */
extern const driver_t *__driver_init_start;
extern const driver_t *__driver_init_end;

/* ============================================================
 * driver_core_init — called early from kernel_main
 *
 * Iterates .driver_init linker section and registers every
 * built-in driver.  Then probes all devices registered by
 * board_init().
 * ============================================================ */
void driver_core_init(void) {
    g_driver_count = 0;
    g_device_count = 0;
    g_bus_count    = 0;

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
    if (!drv || g_driver_count >= MAX_DRIVERS)
        return -1;

    g_drivers[g_driver_count++] = drv;

    kinfo("[DRIVER] registered driver '%s' (class=%d)\n",
          drv->name, drv->class_type);

    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv != NULL)
            continue;
        if (dev->bus && dev->bus->match && dev->bus->match(dev, drv)) {
            dev->drv = drv;
            if (drv->probe) {
                int ret = drv->probe(dev);
                if (ret == 0) {
                    dev->state = DEV_STATE_PROBED;
                    kinfo("[DRIVER] device '%s' bound to driver '%s'\n",
                          dev->name, drv->name);
                } else {
                    dev->drv = NULL;
                    kdebug("[DRIVER] probe '%s' -> '%s' failed: %d\n",
                           dev->name, drv->name, ret);
                }
            }
        }
    }
    return 0;
}

int driver_unregister(driver_t *drv) {
    if (!drv) return -1;
    for (int i = 0; i < g_driver_count; i++) {
        if (g_drivers[i] == drv) {
            for (int j = 0; j < g_device_count; j++) {
                device_t *dev = g_devices[j];
                if (dev->drv == drv) {
                    if (drv->remove) drv->remove(dev);
                    dev->drv   = NULL;
                    dev->state = DEV_STATE_REMOVED;
                }
            }
            g_drivers[i] = g_drivers[--g_driver_count];
            return 0;
        }
    }
    return -1;
}

/* ============================================================
 * device_register — add a device to the system
 *
 * After registration, scans all registered drivers for a match.
 * ============================================================ */
int device_register(device_t *dev) {
    if (!dev || g_device_count >= MAX_DEVICES)
        return -1;

    g_devices[g_device_count++] = dev;
    dev->state = DEV_STATE_UNINIT;

    kinfo("[DRIVER] registered device '%s' (bus=%s)\n",
          dev->name ? dev->name : "?",
          (dev->bus && dev->bus->name) ? dev->bus->name : "?");

    for (int i = 0; i < g_driver_count; i++) {
        driver_t *drv = g_drivers[i];
        if (dev->bus && dev->bus->match && dev->bus->match(dev, drv)) {
            dev->drv = drv;
            if (drv->probe) {
                int ret = drv->probe(dev);
                if (ret == 0) {
                    dev->state = DEV_STATE_PROBED;
                    kinfo("[DRIVER] device '%s' bound to driver '%s'\n",
                          dev->name, drv->name);
                    return 0;
                } else {
                    dev->drv = NULL;
                }
            }
        }
    }
    return 0;
}

void device_unregister(device_t *dev) {
    if (!dev) return;
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i] == dev) {
            if (dev->drv && dev->drv->remove)
                dev->drv->remove(dev);
            dev->drv   = NULL;
            dev->state = DEV_STATE_REMOVED;
            g_devices[i] = g_devices[--g_device_count];
            return;
        }
    }
}

int bus_register(bus_type_t *bus) {
    if (!bus || g_bus_count >= MAX_BUSES)
        return -1;
    g_buses[g_bus_count++] = bus;
    kinfo("[DRIVER] registered bus '%s'\n", bus->name);
    return 0;
}

void bus_unregister(bus_type_t *bus) {
    if (!bus) return;
    for (int i = 0; i < g_bus_count; i++) {
        if (g_buses[i] == bus) {
            g_buses[i] = g_buses[--g_bus_count];
            return;
        }
    }
}

int bus_probe_device(device_t *dev) {
    if (!dev) return -1;
    for (int i = 0; i < g_driver_count; i++) {
        driver_t *drv = g_drivers[i];
        int match = 0;
        if (dev->bus && dev->bus->match) {
            match = dev->bus->match(dev, drv);
        } else if (!dev->bus && !drv->bus) {
            match = 1;
        }
        if (match) {
            dev->drv = drv;
            if (drv->probe) {
                int ret = drv->probe(dev);
                if (ret == 0) {
                    dev->state = DEV_STATE_PROBED;
                    return 0;
                }
                dev->drv = NULL;
            }
        }
    }
    return -1;
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
