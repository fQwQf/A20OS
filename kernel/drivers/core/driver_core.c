/*
 * A20OS Driver Core — registration, matching, enumeration
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_class.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/panic.h"
#include "core/errno.h"
#include "mm/slab.h"
#include "core/cpu.h"

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

/* The scheduler bridge runs driver_progress_class() from every CPU on every
 * pass, and it must never block or skip a device, so it cannot take
 * g_driver_core_ops -- a mutex held across probe/remove -- to walk the
 * registry.  Each CPU therefore keeps a private mirror of the bound devices
 * that have a progress callback, rebuilt under that mutex only when
 * g_progress_epoch moves.  The mirror is a caching hint, never the lifetime
 * guard: entries are device_t pointers, which are static objects that outlive
 * every binding, and each entry is entered through class_device_ref_for_device()
 * plus class_device_call_begin(), so a device unbound mid-walk is either
 * skipped or is pinned until class_device_unpublish() drains it, which happens
 * before drv->remove() frees the driver's private state. */
static device_t **g_progress_mirror[CONFIG_NR_CPUS];
static int         g_progress_count[CONFIG_NR_CPUS];
static int         g_progress_cap[CONFIG_NR_CPUS];
static uint32_t    g_progress_epoch = 1;
static uint32_t    g_progress_mirror_epoch[CONFIG_NR_CPUS];

/* Bumped under g_driver_core_ops whenever a binding changes, so the mirrors
 * know to re-read the registry.  A mirror that has not caught up only costs its
 * CPU one pass of polling a stale device set. */
static void driver_progress_binding_changed(void)
{
    __atomic_add_fetch(&g_progress_epoch, 1, __ATOMIC_RELEASE);
}

static void driver_progress_refresh(unsigned cpu)
{
    int need = g_device_count > 0 ? g_device_count : 1;
    if (need > g_progress_cap[cpu]) {
        device_t **grown = krealloc(g_progress_mirror[cpu],
                                    (size_t)need * sizeof(*grown));
        if (!grown)
            return;
        g_progress_mirror[cpu] = grown;
        g_progress_cap[cpu] = need;
    }
    int n = 0;
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv && dev->drv->progress)
            g_progress_mirror[cpu][n++] = dev;
    }
    __atomic_store_n(&g_progress_count[cpu], n, __ATOMIC_RELEASE);
    __atomic_store_n(&g_progress_mirror_epoch[cpu],
                     __atomic_load_n(&g_progress_epoch, __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
}

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
extern const uintptr_t __driver_init_start;
extern const uintptr_t __driver_init_end;

static int driver_probe_bound_device(driver_t *drv, device_t *dev) {
    dev->drv = drv;
    if (!drv->probe) {
        dev->state = DEV_STATE_PROBED;
        driver_progress_binding_changed();
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
            driver_progress_binding_changed();
            return ret;
        }
        driver_progress_binding_changed();
        return 0;
    }
    /* DRIVER_PROBE_FAILURE_CLEANUP: failed probes leave no half-bound device. */
    dev->drv = NULL;
    dev->drv_priv = NULL;
    dev->matched_id = NULL;
    dev->state = DEV_STATE_UNINIT;
    driver_progress_binding_changed();
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

    /* Seed every CPU's progress mirror before any device is bound.  A mirror
     * is only read once it has a nonzero count, so the initial capacity is a
     * starting point that driver_progress_refresh() grows from. */
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        g_progress_count[cpu] = 0;
        g_progress_cap[cpu] = DEVICE_INITIAL_CAP;
        g_progress_mirror_epoch[cpu] = 0;
        g_progress_mirror[cpu] = kcalloc(DEVICE_INITIAL_CAP, sizeof(device_t *));
        if (!g_progress_mirror[cpu])
            panic("driver_core_init: progress mirror kmalloc failed\n");
    }
    g_progress_epoch = 1;

    /* Iterate the .driver_init pointer table as raw uintptr_t slots.  The
     * section holds pointers to static driver_t objects placed by
     * DRIVER_REGISTER(); memcpy avoids the compiler's UBSAN type-mismatch
     * check on *p, which cannot know the constant-section slots are aligned
     * and reports a false positive here. */
    for (const uintptr_t *p = &__driver_init_start;
         p < &__driver_init_end; p++) {
        uintptr_t slot;
        memcpy(&slot, p, sizeof(slot));
        driver_register((driver_t *)slot);
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
                driver_progress_binding_changed();
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
            driver_progress_binding_changed();
            mutex_unlock(&g_driver_core_ops);
            return;
        }
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    mutex_unlock(&g_driver_core_ops);
}

int device_hotplug(device_t *dev, int event)
{
    if (!dev)
        return -EINVAL;

    if (event == BUS_EVENT_ADD) {
        int ret = device_register(dev);
        if (ret == 0 && dev->bus && dev->bus->hotplug)
            dev->bus->hotplug(dev, BUS_EVENT_ADD);
        return ret;
    }
    if (event == BUS_EVENT_REMOVE) {
        device_unregister(dev);
        if (dev->bus && dev->bus->hotplug)
            dev->bus->hotplug(dev, BUS_EVENT_REMOVE);
        return 0;
    }
    return -EINVAL;
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

int device_set_devfs_name(device_t *dev, const char *name) {
    if (!dev || !name || !name[0])
        return -EINVAL;
    if (dev->class_dev)
        return -EEXIST;  /* already published: too late to rename */
    size_t len = strlen(name);
    if (len >= CLASS_DEVICE_NAME_MAX)
        return -EINVAL;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || c == '/')
            return -EINVAL;
    }
    memcpy(dev->devfs_name, name, len + 1);
    return 0;
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

int driver_lookup_compatible(const char *compatible,
                             uint32_t *vendor, uint32_t *device)
{
    if (!compatible || !compatible[0])
        return -EINVAL;

    const driver_t *owner = NULL;
    int claimants = 0;

    for (int i = 0; i < g_driver_count; i++) {
        const driver_t *drv = g_drivers[i];
        if (!drv->of_compatible || !drv->id_table)
            continue;
        if (strcmp(drv->of_compatible, compatible) != 0)
            continue;
        claimants++;
        if (!owner)
            owner = drv;
    }

    if (!owner)
        return -ENODEV;

    if (claimants > 1)
        kwarn("[DT] \"%s\" claimed by %d drivers, binding %s\n",
              compatible, claimants, owner->name);

    if (vendor)
        *vendor = owner->id_table[0].vendor;
    if (device)
        *device = owner->id_table[0].device;
    return 0;
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
    /* DRIVER_PROGRESS_LIFECYCLE_SERIALIZATION: scheduler/idle progress runs on
     * every CPU and must never block, so it cannot take g_driver_core_ops to
     * walk the registry.  The per-CPU mirror absorbs that walk; the mutex is
     * taken only to rebuild it, and only when a binding has changed since the
     * last rebuild, which never happens on the steady-state path.  The next
     * poll retries if a concurrent lifecycle operation still owns the mutex. */
    unsigned cpu = cpu_current_id();
    if (__atomic_load_n(&g_progress_mirror_epoch[cpu], __ATOMIC_ACQUIRE) !=
            __atomic_load_n(&g_progress_epoch, __ATOMIC_ACQUIRE) &&
        mutex_trylock(&g_driver_core_ops)) {
        driver_progress_refresh(cpu);
        mutex_unlock(&g_driver_core_ops);
    }

    int n = __atomic_load_n(&g_progress_count[cpu], __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        device_t *dev = g_progress_mirror[cpu][i];
        driver_t *drv = dev ? dev->drv : NULL;
        if (!drv || drv->class_type != class_type || !drv->progress)
            continue;
        /* Pin the class device for the callback.  class_device_unpublish()
         * waits for the matching call_end() and runs before drv->remove(),
         * so a driver cannot have its private state freed under a poll that
         * has already entered this window. */
        class_device_t *cdev = class_device_ref_for_device(dev);
        if (!cdev)
            continue;
        if (class_device_call_begin(cdev) == 0) {
            if (dev->drv == drv && drv->progress)
                drv->progress(dev);
            class_device_call_end(cdev);
        }
        class_device_put(cdev);
    }
}

/* Format the registered drivers for /proc/drivers (Linux format). */
int driver_core_list_drivers(char *buf, size_t sz)
{
    if (!buf)
        return -EINVAL;
    size_t off = 0;
    for (int i = 0; i < g_driver_count; i++) {
        driver_t *drv = g_drivers[i];
        if (!drv || !drv->name)
            continue;
        int n = snprintf(buf + off, sz > off ? sz - off : 0, "%s\n",
                         drv->name);
        if (n < 0)
            break;
        off += (size_t)n;
        if (off >= sz)
            break;
    }
    return (int)off;
}
