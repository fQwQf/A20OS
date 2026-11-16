/*
 * A20OS Driver Core — registration, matching, enumeration
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/memory.h"
#include "firmware/device.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_registry_test.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_protocol_internal.h"
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
/* Sticky terminal admission gate. A failed stop permits cleanup retry, not
 * new bindings whose hardware might race the next-kernel handoff. */
static int g_driver_shutdown_active;

/* The scheduler mutex is recursive only when it has a task owner. Before
 * proc_init the boot CPU also enters nested bus/driver callbacks; represent
 * that boot context explicitly instead of recursively taking an ownerless
 * mutex. Registry lifecycle APIs must not be called from interrupt context. */
extern struct task_t *proc_current(void);
static unsigned boot_ops_depth[CONFIG_NR_CPUS];
static void driver_ops_lock(void) {
    if (!proc_current()) {
        unsigned cpu = cpu_current_id();
        if (boot_ops_depth[cpu]++) return;
    }
    mutex_lock(&g_driver_core_ops);
}
static void driver_ops_unlock(void) {
    if (!proc_current()) {
        unsigned cpu = cpu_current_id();
        if (!boot_ops_depth[cpu]) panic("driver lifecycle boot unlock underflow");
        if (--boot_ops_depth[cpu]) return;
    }
    mutex_unlock(&g_driver_core_ops);
}

static driver_t   **g_drivers;
static int         g_driver_count;
static int         g_driver_cap;
static device_t   **g_devices;
static int         g_device_count;
static int         g_device_cap;
static bus_type_t **g_buses;
static int         g_bus_count;
static int         g_bus_cap;

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
/* Consumed by the next registry backing-array growth while the core lock is
 * held.  It does not affect progress mirrors or any unrelated allocation. */
static int g_registry_fail_next_kind;
static int g_registry_test_last_failure_kind;

void driver_registry_test_fail_next(int kind)
{
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    g_registry_fail_next_kind = kind;
    g_registry_test_last_failure_kind = 0;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
}

int driver_registry_test_last_failure(void)
{
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    int kind = g_registry_test_last_failure_kind;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    return kind;
}

void driver_registry_test_snapshot(int kind, int *count, int *capacity)
{
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    switch (kind) {
    case DRIVER_REGISTRY_TEST_DRIVER:
        if (count) *count = g_driver_count;
        if (capacity) *capacity = g_driver_cap;
        break;
    case DRIVER_REGISTRY_TEST_DEVICE:
        if (count) *count = g_device_count;
        if (capacity) *capacity = g_device_cap;
        break;
    case DRIVER_REGISTRY_TEST_BUS:
        if (count) *count = g_bus_count;
        if (capacity) *capacity = g_bus_cap;
        break;
    default:
        if (count) *count = -1;
        if (capacity) *capacity = -1;
        break;
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
}
#endif

static void *driver_registry_realloc(void *old, size_t size, int kind)
{
#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
    if (g_registry_fail_next_kind == kind) {
        g_registry_fail_next_kind = 0;
        g_registry_test_last_failure_kind = kind;
        return NULL;
    }
#else
    (void)kind;
#endif
    return krealloc(old, size);
}

#ifdef CONFIG_DRIVER_REGISTRY_RUST
static void *driver_registry_grow_driver(void *old, size_t size)
{
    return driver_registry_realloc(old, size, DRIVER_REGISTRY_TEST_DRIVER);
}

static void *driver_registry_grow_device(void *old, size_t size)
{
    return driver_registry_realloc(old, size, DRIVER_REGISTRY_TEST_DEVICE);
}

static void *driver_registry_grow_bus(void *old, size_t size)
{
    return driver_registry_realloc(old, size, DRIVER_REGISTRY_TEST_BUS);
}
#endif

/* The scheduler bridge runs driver_progress_class() from every CPU on every
 * pass, and it must never block or skip a device, so it cannot take
 * g_driver_core_ops -- a mutex held across probe/remove -- to walk the
 * registry.  Each CPU therefore keeps a private mirror of the bound devices
 * that have a progress callback, rebuilt under that mutex only when
 * g_progress_epoch moves.  The mirror is a caching hint, never the lifetime
 * guard: entries hold device object references, and each entry is entered
 * through class_device_ref_for_device()
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

device_t *device_get(device_t *dev) {
    if (!dev) return NULL;
    unsigned old = __atomic_load_n(&dev->references, __ATOMIC_RELAXED);
    while (old && old != (unsigned)-1) {
        if (__atomic_compare_exchange_n(&dev->references, &old, old + 1, 0,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return dev;
    }
    return NULL;
}
void device_put(device_t *dev) {
    if (!dev) return;
    if (__atomic_sub_fetch(&dev->references, 1, __ATOMIC_ACQ_REL) == 0) {
        device_t *parent = dev->parent_reference;
        dev->parent_reference = NULL;
        if (dev->release) dev->release(dev);
        device_put(parent);
    }
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
    for (int i = 0; i < g_progress_count[cpu]; i++)
        device_put(g_progress_mirror[cpu][i]);
    int n = 0;
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv && dev->drv->progress && device_get(dev))
            g_progress_mirror[cpu][n++] = dev;
    }
    __atomic_store_n(&g_progress_count[cpu], n, __ATOMIC_RELEASE);
    __atomic_store_n(&g_progress_mirror_epoch[cpu],
                     __atomic_load_n(&g_progress_epoch, __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
}

/* Lifecycle intent is stack-scoped under driver_ops_lock, without a public
 * layout change. Unrelated nested teardowns can still make progress. */
struct driver_withdrawal { driver_t *driver; struct driver_withdrawal *previous; };
static struct driver_withdrawal *driver_withdrawals;
struct device_removal { device_t *device; struct device_removal *previous; };
static struct device_removal *device_removals;
struct device_probe {
    device_t *device;
    driver_t *driver;
    bool remove_on_error;
    struct device_probe *previous;
};
static struct device_probe *device_probes;
static bool device_probe_active(device_t *dev) {
    for (struct device_probe *at = device_probes; at; at = at->previous)
        if (at->device == dev) return true;
    return false;
}
static bool device_removal_active(device_t *dev) {
    for (struct device_removal *at = device_removals; at; at = at->previous)
        if (at->device == dev) return true;
    return false;
}
static bool driver_withdrawing(driver_t *drv) {
    for (struct driver_withdrawal *at = driver_withdrawals; at; at = at->previous)
        if (at->driver == drv) return true;
    return false;
}

static int driver_matches_device(driver_t *drv, device_t *dev)
{
    int match = 0;
    dev->matched_id = NULL;
    dev->firmware_match = NULL;
    if (g_driver_shutdown_active || driver_withdrawing(drv) ||
        device_is_stopping(dev)) return 0;
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
        dev->firmware_match = NULL;
        match = 0;
    }
    /* One owner per device: a user-owned device accepts only read-only
     * kernel probes; the owning user-service driver drives it. */
    if (match && dev->user_owned && !drv->read_only_probe) {
        dev->matched_id = NULL;
        dev->firmware_match = NULL;
        match = 0;
    }
    return match;
}

/* ---- Linker-generated section boundaries for built-in drivers ---- */
extern const uintptr_t __driver_init_start;
extern const uintptr_t __driver_init_end;

static unsigned discovery_depth;

struct device_dependency {
    device_t *provider;
    struct device_dependency *next;
};
struct device_resource_action {
    void (*release)(void *);
    void *data;
    struct device_resource_action *next;
    int (*prepare)(void *);
    bool prepared;
    int (*stop)(void *);
    bool stopped;
};
struct protocol_action {
    struct device_resource_action action;
    struct driver_protocol_descriptor descriptor;
};
static void release_protocol_metadata(void *data) { (void)data; }
/* Probe/publish lifecycle owner only. Metadata lives in the existing devres
 * list; publication copies it before any consumer can enter this binding. */
size_t device_protocols_copy(const device_t *dev, struct driver_protocol_descriptor *out)
{
    size_t count = 0;
    for (const struct device_resource_action *a = dev->resource_actions; a; a = a->next)
        if (a->release == release_protocol_metadata) {
            if (out) out[count] = *(const struct driver_protocol_descriptor *)a->data;
            count++;
        }
    return count;
}
int device_add_resource_action(device_t *dev, void (*release)(void *), void *data) {
    if (!dev || !release) return -EINVAL;
    struct device_resource_action *action = kmalloc(sizeof(*action));
    if (!action) return -ENOMEM;
    *action = (struct device_resource_action){.release = release, .data = data,
                                               .next = dev->resource_actions};
    dev->resource_actions = action;
    return 0;
}
int device_release_resource_action(device_t *dev, void (*release)(void *), void *data) {
    if (!dev || !release) return -EINVAL;
    struct device_resource_action **link = &dev->resource_actions;
    while (*link) {
        struct device_resource_action *action = *link;
        if (action->release != release || action->data != data) {
            link = &action->next;
            continue;
        }
        *link = action->next;
        release(data);
        kfree(action);
        return 0;
    }
    return -ENOENT;
}

void device_release_resources(device_t *dev) {
    while (dev && dev->resource_actions) {
        struct device_resource_action *action = dev->resource_actions;
        dev->resource_actions = action->next;
        if (action->release) action->release(action->data);
        kfree(action);
    }
}
struct driver_resource_claim {
    device_t *owner;
    enum resource_type type;
    uint64_t start, end;
    bool checked;
    struct driver_resource_claim *next;
};
/* Protected by the registry spinlock. Allocation, owner finalizers and MMIO
 * callbacks stay outside that lock. Claims track active users, not every
 * firmware declaration or an enclosing host-bridge aperture. */
static driver_resource_claim_t *resource_claims;

struct driver_memory_reservation {
    struct driver_memory_reservation *next;
    size_t count;
    driver_memory_range_t ranges[];
};
/* Same registry lock as claims. Storage cannot become reserved in the gap
 * between a mapper's conflict check and claim publication (or vice versa). */
static driver_memory_reservation_t *memory_reservations;

static bool reservation_conflict(const driver_memory_reservation_t *reservation,
                                 uint64_t start, uint64_t end,
                                 uint64_t *busy_end) {
    /* Sorted, disjoint ranges: the last start <= end has the greatest end
     * among possible conflicts. No linear firmware-table scan with IRQs off. */
    size_t low = 0, high = reservation->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (reservation->ranges[mid].start <= end)
            low = mid + 1;
        else
            high = mid;
    }
    if (!low || reservation->ranges[low - 1].end < start)
        return false;
    uint64_t last = reservation->ranges[low - 1].end;
    if (busy_end && last > *busy_end)
        *busy_end = last;
    return true;
}

static bool memory_conflict(uint64_t start, uint64_t end, uint64_t *busy_end) {
    bool busy = false;
    for (driver_memory_reservation_t *at = memory_reservations; at;
         at = at->next)
        busy |= reservation_conflict(at, start, end, busy_end);
    return busy;
}

int driver_memory_reserve(const driver_memory_range_t *ranges, size_t count,
                          driver_memory_reservation_t **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!ranges || !count)
        return -EINVAL;
    if (count >
        (~(size_t)0 - sizeof(driver_memory_reservation_t)) / sizeof(*ranges))
        return -EOVERFLOW;
    driver_memory_reservation_t *reservation =
        kmalloc(sizeof(*reservation) + count * sizeof(*ranges));
    if (!reservation)
        return -ENOMEM;
    reservation->count = count;
    memcpy(reservation->ranges, ranges, count * sizeof(*ranges));
    for (size_t i = 0; i < count; i++)
        if (reservation->ranges[i].start > reservation->ranges[i].end) {
            kfree(reservation);
            return -EINVAL;
        }
    /* Normalize copied input outside both locks. Adjacent/overlapping table
     * storage can share a reservation, without inventing parent ownership. */
    for (size_t i = 1; i < count; i++) {
        driver_memory_range_t range = reservation->ranges[i];
        size_t j = i;
        while (j && reservation->ranges[j - 1].start > range.start) {
            reservation->ranges[j] = reservation->ranges[j - 1];
            j--;
        }
        reservation->ranges[j] = range;
    }
    size_t merged = 0;
    for (size_t i = 0; i < count; i++) {
        driver_memory_range_t range = reservation->ranges[i];
        driver_memory_range_t *previous =
            merged ? &reservation->ranges[merged - 1] : NULL;
        if (previous && (range.start <= previous->end ||
                         (previous->end != ~(uint64_t)0 &&
                          range.start == previous->end + 1))) {
            if (range.end > previous->end)
                previous->end = range.end;
        } else
            reservation->ranges[merged++] = range;
    }
    reservation->count = merged;
    driver_ops_lock();
    int ret = 0;
    for (int i = 0; i < g_device_count && !ret; i++) {
        const device_t *dev = g_devices[i];
        if (dev->res_count < 0 || (dev->res_count && !dev->res)) {
            ret = -EINVAL;
            break;
        }
        for (int j = 0; j < dev->res_count && !ret; j++) {
            const resource_t *res = &dev->res[j];
            if (res->type != RES_MMIO)
                continue;
            if (res->start > res->end) {
                ret = -EINVAL;
                break;
            }
            if (reservation_conflict(reservation, res->start, res->end, NULL))
                ret = -EBUSY;
        }
    }
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (driver_resource_claim_t *at = resource_claims; at && !ret;
         at = at->next) {
        if (at->type != RES_MMIO)
            continue;
        if (reservation_conflict(reservation, at->start, at->end, NULL))
            ret = -EBUSY;
    }
    if (!ret) {
        reservation->next = memory_reservations;
        memory_reservations = reservation;
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    driver_ops_unlock();
    if (ret)
        kfree(reservation);
    else
        *out = reservation;
    return ret;
}

void driver_memory_release(driver_memory_reservation_t *reservation) {
    if (!reservation)
        return;
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    driver_memory_reservation_t **at = &memory_reservations;
    while (*at && *at != reservation)
        at = &(*at)->next;
    if (!*at) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        panic("releasing an unowned driver memory reservation");
    }
    *at = reservation->next;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    kfree(reservation);
}
static int resource_request(device_t *owner, const resource_t *res,
                            driver_resource_claim_t **out, bool checked) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!owner || !res || (res->type != RES_MMIO && res->type != RES_IOPORT) ||
        res->end < res->start || (res->type == RES_IOPORT && res->end > 0xffff))
        return -EINVAL;
    driver_resource_claim_t *claim = kmalloc(sizeof(*claim));
    if (!claim)
        return -ENOMEM;
    if (!device_get(owner)) {
        kfree(claim);
        return -ENODEV;
    }
    *claim = (driver_resource_claim_t){owner,    res->type, res->start,
                                       res->end, checked,   NULL};
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    if (claim->type == RES_MMIO &&
        memory_conflict(claim->start, claim->end, NULL)) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        device_put(owner);
        kfree(claim);
        return -EBUSY;
    }
    for (driver_resource_claim_t *at = resource_claims; at; at = at->next) {
        if (at->owner != owner && at->type == claim->type &&
            at->start <= claim->end && claim->start <= at->end) {
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            device_put(owner);
            kfree(claim);
            return -EBUSY;
        }
    }
    claim->next = resource_claims;
    resource_claims = claim;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    *out = claim;
    return 0;
}

int driver_resource_request(device_t *owner, const resource_t *res,
                            driver_resource_claim_t **out) {
    return resource_request(owner, res, out, false);
}

static enum resource_type resource_space(enum resource_type type) {
    return type == RES_MEM ? RES_MMIO : type;
}
static bool valid_range(const resource_t *res) {
    return res &&
           (resource_space(res->type) == RES_MMIO || res->type == RES_IOPORT) &&
           res->start <= res->end &&
           (res->type != RES_IOPORT || res->end <= 0xffff);
}
/* Lifecycle mutex held: declarations cannot be published/withdrawn or changed
 * by a cooperating bus. The claim list additionally needs the registry lock. */
static int check_range(device_t *owner, const resource_t *res,
                       uint64_t *busy_end) {
    uint64_t end = 0;
    bool busy = false;
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev == owner)
            continue;
        if (dev->res_count < 0 || (dev->res_count && !dev->res))
            return -EINVAL;
        for (int j = 0; j < dev->res_count; j++) {
            const resource_t *at = &dev->res[j];
            if (resource_space(at->type) != resource_space(res->type))
                continue;
            if (!valid_range(at))
                return -EINVAL;
            if (at->start <= res->end && res->start <= at->end) {
                if (at->end > end)
                    end = at->end;
                busy = true;
            }
        }
    }
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    if (resource_space(res->type) == RES_MMIO)
        busy |= memory_conflict(res->start, res->end, &end);
    for (driver_resource_claim_t *at = resource_claims; at; at = at->next) {
        if (at->owner != owner && at->type == resource_space(res->type) &&
            at->start <= res->end && res->start <= at->end) {
            if (at->end > end)
                end = at->end;
            busy = true;
        }
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    if (busy && busy_end)
        *busy_end = end;
    return busy ? -EBUSY : 0;
}
int driver_resource_check_range(device_t *owner, const resource_t *res,
                                uint64_t *busy_end) {
    if (busy_end)
        *busy_end = 0;
    if (!valid_range(res))
        return -EINVAL;
    driver_ops_lock();
    int ret = check_range(owner, res, busy_end);
    driver_ops_unlock();
    return ret;
}
int driver_resource_check_claims(device_t *owner, const resource_t *res) {
    if (!valid_range(res))
        return -EINVAL;
    int ret = 0;
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    if (resource_space(res->type) == RES_MMIO &&
        memory_conflict(res->start, res->end, NULL))
        ret = -EBUSY;
    for (driver_resource_claim_t *at = resource_claims; at; at = at->next) {
        if (at->owner != owner && at->type == resource_space(res->type) &&
            at->start <= res->end && res->start <= at->end) {
            ret = -EBUSY;
            break;
        }
    }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    return ret;
}
bool device_is_registered(const device_t *dev) {
    if (!dev)
        return false;
    bool found = false;
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_device_count; i++)
        if (g_devices[i] == dev) {
            found = true;
            break;
        }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    return found;
}

int device_probe_use_remove(device_t *dev) {
    if (!dev) return -EINVAL;
    driver_ops_lock();
    struct device_probe *probe = device_probes;
    int ret = -EINVAL;
    if (probe && probe->device == dev && probe->driver == dev->drv) {
        ret = !device_is_registered(dev) ? -ENODEV :
              !probe->driver->remove ? -EOPNOTSUPP : 0;
        if (!ret) probe->remove_on_error = true;
    }
    driver_ops_unlock();
    return ret;
}

int device_add_protocol(device_t *dev, const struct driver_protocol_descriptor *descriptor)
{
    if (!dev || !descriptor || !descriptor->info.id || !descriptor->info.version ||
        !descriptor->info.ops_size || !descriptor->ops) return -EINVAL;
    driver_ops_lock();
    struct device_probe *probe = device_probes;
    int ret = -EINVAL;
    if (!probe || probe->device != dev || probe->driver != dev->drv) goto done;
    if (!device_is_registered(dev)) { ret = -ENODEV; goto done; }
    if (dev->drv->class_ops && descriptor->info.id == driver_protocol_class_id(dev->drv->class_type)) {
        ret = -EEXIST; goto done;
    }
    for (struct device_resource_action *a = dev->resource_actions; a; a = a->next)
        if (a->release == release_protocol_metadata &&
            ((const struct driver_protocol_descriptor *)a->data)->info.id == descriptor->info.id) {
            ret = -EEXIST; goto done;
        }
    struct protocol_action *entry = kmalloc(sizeof(*entry));
    if (!entry) { ret = -ENOMEM; goto done; }
    entry->descriptor = *descriptor;
    entry->action = (struct device_resource_action){
        .release = release_protocol_metadata, .data = &entry->descriptor,
        .next = dev->resource_actions,
    };
    dev->resource_actions = &entry->action;
    ret = 0;
done:
    driver_ops_unlock();
    return ret;
}

int device_add_remove_prepare(device_t *dev, int (*prepare)(void *), void *context) {
    if (!dev || !prepare) return -EINVAL;
    driver_ops_lock();
    struct device_probe *probe = device_probes;
    int ret = -EINVAL;
    if (probe && probe->device == dev && probe->driver == dev->drv &&
        probe->remove_on_error) {
        ret = device_is_registered(dev) ? 0 : -ENODEV;
        if (!ret) {
            struct device_resource_action *action = kmalloc(sizeof(*action));
            if (!action) ret = -ENOMEM;
            else {
                *action = (struct device_resource_action){.data = context,
                    .next = dev->resource_actions, .prepare = prepare};
                dev->resource_actions = action;
            }
        }
    }
    driver_ops_unlock();
    return ret;
}

int device_add_execution_action(device_t *dev, int (*stop)(void *),
                                void (*release)(void *), void *context) {
    if (!dev || !stop) return -EINVAL;
    driver_ops_lock();
    struct device_probe *probe = device_probes;
    int ret = -EINVAL;
    if (probe && probe->device == dev && probe->driver == dev->drv) {
        ret = !device_is_registered(dev) ? -ENODEV :
              !probe->driver->remove ? -EOPNOTSUPP : 0;
        if (!ret) {
            /* Arm rollback before allocation: OOM must not select legacy
             * failed-probe resource release after hardware initialization. */
            probe->remove_on_error = true;
            struct device_resource_action *action = kmalloc(sizeof(*action));
            if (!action) ret = -ENOMEM;
            else {
                *action = (struct device_resource_action){.stop = stop,
                    .release = release, .data = context, .next = dev->resource_actions};
                dev->resource_actions = action;
            }
        }
    }
    driver_ops_unlock();
    return ret;
}

int driver_resource_request_checked(device_t *owner, const resource_t *res,
                                    driver_resource_claim_t **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!owner || !valid_range(res) || res->type == RES_MEM)
        return -EINVAL;
    driver_ops_lock();
    bool registered = device_is_registered(owner);
    int ret = registered && !device_is_stopping(owner)
                  ? check_range(owner, res, NULL)
                  : -ENODEV;
    if (!ret)
        ret = resource_request(owner, res, out, true);
    driver_ops_unlock();
    return ret;
}
/* Registry spinlock and lifecycle mutex held. Only checked claims restrict
 * publication; basic claims retain their original cooperative semantics. */
static int declaration_conflict(device_t *dev) {
    if (dev->res_count < 0 || (dev->res_count && !dev->res))
        return -EINVAL;
    for (int i = 0; i < dev->res_count; i++) {
        const resource_t *res = &dev->res[i];
        if (res->type == RES_MMIO && memory_reservations) {
            if (res->start > res->end)
                return -EINVAL;
            if (memory_conflict(res->start, res->end, NULL))
                return -EBUSY;
        }
        if (resource_space(res->type) != RES_MMIO && res->type != RES_IOPORT)
            continue;
        for (driver_resource_claim_t *at = resource_claims; at; at = at->next) {
            if (!at->checked || at->owner == dev ||
                at->type != resource_space(res->type))
                continue;
            if (!valid_range(res))
                return -EINVAL;
            if (at->start <= res->end && res->start <= at->end)
                return -EBUSY;
        }
    }
    return 0;
}

void driver_resource_release(driver_resource_claim_t *claim) {
    if (!claim)
        return;
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    driver_resource_claim_t **at = &resource_claims;
    while (*at && *at != claim)
        at = &(*at)->next;
    if (!*at) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        panic("releasing an unowned driver resource claim");
    }
    *at = claim->next;
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    device_t *owner = claim->owner;
    kfree(claim);
    device_put(owner);
}

static bool resource_belongs(device_t *dev, const resource_t *res) {
    if (!dev || !res || !dev->res)
        return false;
    for (int i = 0; i < dev->res_count; i++)
        if (&dev->res[i] == res)
            return true;
    return false;
}
static void release_claim(void *data) {
    driver_resource_release(data);
}
int device_claim_resource(device_t *dev, const resource_t *res) {
    if (!resource_belongs(dev, res))
        return -EINVAL;
    driver_resource_claim_t *claim;
    int ret = driver_resource_request(dev, res, &claim);
    if (ret)
        return ret;
    ret = device_add_resource_action(dev, release_claim, claim);
    if (ret)
        driver_resource_release(claim);
    return ret;
}

struct device_mmio_mapping {
    void *address;
    size_t size;
    driver_resource_claim_t *claim;
};
static void release_mmio(void *data) {
    struct device_mmio_mapping *map = data;
    arch_device_unmap_mmio(map->address, map->size);
    driver_resource_release(map->claim);
    kfree(map);
}
int device_map_resource_exclusive(device_t *dev, const resource_t *res,
                                  void **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!resource_belongs(dev, res) || res->type != RES_MMIO ||
        res->end < res->start)
        return -EINVAL;
    if (res->end - res->start >= SIZE_MAX)
        return -EOVERFLOW;
    size_t size = (size_t)(res->end - res->start + 1);
    struct device_mmio_mapping *map = kmalloc(sizeof(*map));
    if (!map)
        return -ENOMEM;
    int ret = driver_resource_request(dev, res, &map->claim);
    if (ret) {
        kfree(map);
        return ret;
    }
    map->address = arch_device_map_mmio(res->start, size, res->flags);
    map->size = size;
    if (!map->address) {
        driver_resource_release(map->claim);
        kfree(map);
        return -ENOMEM;
    }
    ret = device_add_resource_action(dev, release_mmio, map);
    if (ret) {
        release_mmio(map);
        return ret;
    }
    *out = map->address;
    return 0;
}
void *device_map_resource(device_t *dev, const resource_t *res) {
    void *address;
    return device_map_resource_exclusive(dev, res, &address) ? NULL : address;
}
void *device_map_mmio(device_t *dev, int index) {
    return device_map_resource(dev, device_get_resource(dev, RES_MMIO, index));
}
static int depends_on(device_t *dev, device_t *target, unsigned depth) {
    if (dev == target) return 1;
    if (depth > (unsigned)g_device_count) return 1;
    if (dev->parent && depends_on(dev->parent, target, depth + 1)) return 1;
    for (struct device_dependency *link = dev->dependencies; link; link = link->next)
        if (depends_on(link->provider, target, depth + 1)) return 1;
    return 0;
}
int device_add_dependency(device_t *consumer, device_t *provider) {
    if (!consumer || !provider) return -EINVAL;
    driver_ops_lock();
    if (g_driver_shutdown_active) {
        driver_ops_unlock(); return -ESHUTDOWN;
    }
    if (device_is_stopping(provider) || device_removal_active(provider)) {
        driver_ops_unlock(); return -EPROBE_DEFER;
    }
    if (depends_on(provider, consumer, 0)) {
        driver_ops_unlock(); return -ELOOP;
    }
    for (struct device_dependency *link = consumer->dependencies; link; link = link->next)
        if (link->provider == provider) { driver_ops_unlock(); return 0; }
    struct device_dependency *link = kmalloc(sizeof(*link));
    if (!link) { driver_ops_unlock(); return -ENOMEM; }
    if (!device_get(provider)) {
        kfree(link); driver_ops_unlock(); return -ENODEV;
    }
    *link = (struct device_dependency){provider, consumer->dependencies};
    consumer->dependencies = link;
    driver_ops_unlock();
    return 0;
}
static void release_dependencies(device_t *dev) {
    while (dev->dependencies) {
        struct device_dependency *link = dev->dependencies;
        dev->dependencies = link->next;
        device_put(link->provider);
        kfree(link);
    }
}
/* Called with the lifecycle mutex held. Consumers are quiesced before any
 * provider resource is released; the acyclic link graph bounds recursion. */
static int device_unbind(device_t *dev) {
    if (device_removal_active(dev) || device_probe_active(dev)) return -EBUSY;
    struct device_removal removal = {dev, device_removals};
    device_removals = &removal;
    int result = 0;
    if (dev->state == DEV_STATE_PROBE_CLEANUP)
        class_device_mark_offline(dev);
    /* An opt-in provider may need to freeze hardware before its consumers
     * can release DMA-visible storage. Checkpoint each successful action. */
    for (struct device_resource_action *action = dev->resource_actions;
         action; action = action->next) {
        if (!action->prepare) continue;
        if (dev->state != DEV_STATE_REMOVING && dev->state != DEV_STATE_PROBE_CLEANUP)
            dev->state = DEV_STATE_PREPARING;
        class_device_mark_offline(dev);
        if (!action->prepared) {
            int ret = action->prepare(action->data);
            if (ret) { result = ret < 0 ? ret : -EIO; goto done; }
            action->prepared = true;
        }
    }
    for (;;) {
        device_t *dependent = NULL;
        for (int i = 0; i < g_device_count && !dependent; i++) {
            device_t *consumer = g_devices[i];
            if (consumer == dev) continue;
            if (consumer->drv && consumer->parent == dev) dependent = consumer;
            for (struct device_dependency *link = consumer->dependencies; link; link = link->next)
                if (link->provider == dev) { dependent = consumer; break; }
        }
        if (!dependent) break;
        device_get(dependent);
        int ret = device_unbind(dependent);
        if (ret) {
            device_put(dependent);
            result = ret;
            goto done; /* Keep the provider available to the failed consumer. */
        }
        dependent->probe_error = -EPROBE_DEFER;
        dependent->state = DEV_STATE_DEFERRED;
        device_put(dependent);
    }
    /* Execution must stop before a synchronous class call can be drained:
     * it may be waiting for one of these requests. Attempt every independent
     * action, checkpoint successes and return failures without entering the
     * legacy unbounded call drain. Never release storage in this phase. */
    for (struct device_resource_action *action = dev->resource_actions;
         action; action = action->next) {
        if (!action->stop || action->stopped) continue;
        if (dev->state != DEV_STATE_REMOVING && dev->state != DEV_STATE_PROBE_CLEANUP)
            dev->state = DEV_STATE_PREPARING;
        class_device_mark_offline(dev);
        int ret = action->stop(action->data);
        if (!ret) action->stopped = true;
        else if (!result) result = ret < 0 ? ret : -EIO;
    }
    if (result) goto done;
    driver_t *drv = dev->drv;
    if (dev->state != DEV_STATE_REMOVING) {
        dev->state = DEV_STATE_REMOVING;
        class_device_mark_offline(dev);
        if (drv && drv->quiesce)
            drv->quiesce(dev);
        class_device_unpublish(dev);
    }
    if (drv && drv->remove) {
        int ret = drv->remove(dev);
        if (ret) {
            ret = ret < 0 ? ret : -EIO;
            kerr("[DRIVER] remove '%s' from '%s' failed: %d; binding retained\n",
                 drv->name, dev->name, ret);
            result = ret;
            goto done;
        }
    }
    device_release_resources(dev);
    release_dependencies(dev);
    dev->drv = NULL; dev->drv_priv = NULL;
    dev->matched_id = NULL; dev->firmware_match = NULL;
    dev->state = DEV_STATE_REMOVED;
    driver_progress_binding_changed();
done:
    device_removals = removal.previous;
    return result;
}

static int driver_probe_bound_device(driver_t *drv, device_t *dev) {
    /* A retry reconstructs its links. A different matching driver must not
     * inherit dependencies retained by an earlier deferred probe. */
    release_dependencies(dev);
    dev->drv = drv;
    int prepared = firmware_device_prepare(dev);
    if (prepared) {
        dev->drv = NULL;
        dev->matched_id = NULL;
        dev->firmware_match = NULL;
        if (prepared != -EPROBE_DEFER) release_dependencies(dev);
        dev->probe_error = prepared;
        dev->state = prepared == -EPROBE_DEFER ? DEV_STATE_DEFERRED : DEV_STATE_UNINIT;
        return prepared;
    }
    struct device_probe probe = {dev, drv, false, device_probes};
    device_probes = &probe;
    int ret = drv->probe ? drv->probe(dev) : 0;
    device_probes = probe.previous;
    if (ret && probe.remove_on_error) {
        int original = ret < 0 ? ret : -EIO;
        dev->probe_error = original;
        dev->state = DEV_STATE_PROBE_CLEANUP;
        int cleanup = device_unbind(dev);
        dev->probe_error = original;
        if (!cleanup)
            dev->state = original == -EPROBE_DEFER ? DEV_STATE_DEFERRED : DEV_STATE_UNINIT;
        driver_progress_binding_changed();
        return cleanup ? cleanup : original;
    }
    dev->probe_error = ret;
    if (ret == 0) {
        dev->state = DEV_STATE_PROBED;
        ret = class_device_publish(dev);
        if (ret < 0) {
            int cleanup = device_unbind(dev);
            dev->probe_error = ret;
            if (cleanup) return cleanup;
            dev->state = DEV_STATE_UNINIT;
            driver_progress_binding_changed();
            return ret;
        }
        driver_progress_binding_changed();
        return 0;
    }
    /* DRIVER_PROBE_FAILURE_CLEANUP: failed probes leave no half-bound device. */
    device_release_resources(dev);
    if (ret != -EPROBE_DEFER) release_dependencies(dev);
    dev->drv = NULL;
    dev->drv_priv = NULL;
    dev->matched_id = NULL;
    dev->firmware_match = NULL;
    dev->state = ret == -EPROBE_DEFER ? DEV_STATE_DEFERRED : DEV_STATE_UNINIT;
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

    driver_ops_lock();
    if (g_driver_shutdown_active) {
        driver_ops_unlock(); return -ESHUTDOWN;
    }
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
#ifdef CONFIG_DRIVER_REGISTRY_RUST
    int insert = a20_driver_registry_insert((void **)&g_drivers,
        &g_driver_count, &g_driver_cap, drv, driver_registry_grow_driver);
    if (insert == -EEXIST) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return insert;
    }
    if (insert < 0) {
        kerr("[DRIVER] REGISTRY_CAPACITY_EXHAUSTED kind=driver capacity=%d error=%d\n",
             g_driver_cap, insert);
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return insert;
    }
#else
    for (int i = 0; i < g_driver_count; i++) {
        if (g_drivers[i] == drv) {
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            driver_ops_unlock();
            return -EEXIST;
        }
    }
    if (g_driver_count >= g_driver_cap) {
        int new_cap = g_driver_cap * 2;
        driver_t **new_arr = driver_registry_realloc(
            g_drivers, sizeof(*g_drivers) * (size_t)new_cap,
            DRIVER_REGISTRY_TEST_DRIVER);
        if (!new_arr) {
            kerr("[DRIVER] REGISTRY_CAPACITY_EXHAUSTED kind=driver capacity=%d error=%d\n",
                 g_driver_cap, -ENOMEM);
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            driver_ops_unlock();
            return -ENOMEM;
        }
        g_drivers = new_arr;
        g_driver_cap = new_cap;
    }

    g_drivers[g_driver_count++] = drv;
#endif
    spin_unlock_irqrestore(&g_driver_core_lock, flags);

    kinfo("[DRIVER] registered driver '%s' (class=%d)\n",
          drv->name, drv->class_type);

    for (int i = 0; !discovery_depth && i < g_device_count; i++) {
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
    driver_ops_unlock();
    driver_retry_deferred();
    return 0;
}

int driver_unregister(driver_t *drv) {
    if (!drv) return -EINVAL;
    driver_ops_lock();
    if (driver_withdrawing(drv)) { driver_ops_unlock(); return -EBUSY; }
    int registered = 0;
    for (int i = 0; i < g_driver_count; i++)
        if (g_drivers[i] == drv) { registered = 1; break; }
    if (!registered) { driver_ops_unlock(); return -ENOENT; }
    struct driver_withdrawal withdrawal = {drv, driver_withdrawals};
    driver_withdrawals = &withdrawal;
    int ret = 0;
    for (;;) {
        device_t *bound = NULL;
        for (int j = 0; j < g_device_count; j++)
            if (g_devices[j]->drv == drv) { bound = g_devices[j]; break; }
        if (!bound) break;
        ret = device_unbind(bound);
        if (ret) goto done;
    }
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
#ifdef CONFIG_DRIVER_REGISTRY_RUST
    ret = a20_driver_registry_remove((void **)&g_drivers, &g_driver_count, drv);
#else
    for (int i = 0; i < g_driver_count; i++) {
        if (g_drivers[i] == drv) {
            g_drivers[i] = g_drivers[--g_driver_count];
            break;
        }
    }
#endif
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
done:
    driver_withdrawals = withdrawal.previous;
    driver_ops_unlock();
    return ret;
}

/* ============================================================
 * device_register — add a device to the system
 *
 * After registration, scans all registered drivers for a match.
 * ============================================================ */
int device_register(device_t *dev) {
    if (!dev || !dev->name)
        return -EINVAL;

    driver_ops_lock();
    if (g_driver_shutdown_active) {
        driver_ops_unlock(); return -ESHUTDOWN;
    }
    device_t *held_parent = NULL;
    if (dev->parent_reference && dev->parent_reference != dev->parent) {
        driver_ops_unlock();
        return -EBUSY;
    }
    if (dev->parent) {
        device_t *at = dev->parent;
        unsigned depth = 0;
        while (at && at != dev && depth++ <= (unsigned)g_device_count) {
            if (device_is_stopping(at) || device_removal_active(at)) {
                driver_ops_unlock(); return -EBUSY;
            }
            at = at->parent;
        }
        if (at) { driver_ops_unlock(); return -ELOOP; }
        if (!dev->parent_reference) {
            held_parent = device_get(dev->parent);
            if (!held_parent) { driver_ops_unlock(); return -ENODEV; }
        }
    }
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    int conflict = declaration_conflict(dev);
    if (conflict) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        device_put(held_parent);
        return conflict;
    }
#ifdef CONFIG_DRIVER_REGISTRY_RUST
    int insert = a20_driver_registry_insert((void **)&g_devices,
        &g_device_count, &g_device_cap, dev, driver_registry_grow_device);
    if (insert == -EEXIST) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        device_put(held_parent);
        return insert;
    }
    if (insert < 0) {
        kerr("[DRIVER] REGISTRY_CAPACITY_EXHAUSTED kind=device capacity=%d error=%d\n",
             g_device_cap, insert);
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        device_put(held_parent);
        return insert;
    }
    dev->state = DEV_STATE_UNINIT;
#else
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i] == dev) {
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            driver_ops_unlock();
            device_put(held_parent);
            return -EEXIST;
        }
    }
    if (g_device_count >= g_device_cap) {
        int new_cap = g_device_cap * 2;
        device_t **new_arr = driver_registry_realloc(
            g_devices, sizeof(*g_devices) * (size_t)new_cap,
            DRIVER_REGISTRY_TEST_DEVICE);
        if (!new_arr) {
            kerr("[DRIVER] REGISTRY_CAPACITY_EXHAUSTED kind=device capacity=%d error=%d\n",
                 g_device_cap, -ENOMEM);
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            driver_ops_unlock();
            device_put(held_parent);
            return -ENOMEM;
        }
        g_devices = new_arr;
        g_device_cap = new_cap;
    }
    g_devices[g_device_count++] = dev;
    dev->state = DEV_STATE_UNINIT;
#endif
    if (held_parent) dev->parent_reference = held_parent;
    if (!dev->references) __atomic_store_n(&dev->references, 1, __ATOMIC_RELAXED);
    if (!device_get(dev)) panic("registering retired device");
    spin_unlock_irqrestore(&g_driver_core_lock, flags);

    kinfo("[DRIVER] registered device '%s' (bus=%s)\n",
          dev->name ? dev->name : "?",
          (dev->bus && dev->bus->name) ? dev->bus->name : "?");

    for (int i = 0; !discovery_depth && i < g_driver_count; i++) {
        driver_t *drv = g_drivers[i];
        if (driver_matches_device(drv, dev)) {
            int ret = driver_probe_bound_device(drv, dev);
            if (ret == 0) {
                kinfo("[DRIVER] device '%s' bound to driver '%s'\n",
                      dev->name, drv->name);
                driver_ops_unlock();
                return 0;
            }
            if (ret == -EPROBE_DEFER || dev->drv) break;
        }
    }
    driver_ops_unlock();
    return 0;
}

int device_unregister(device_t *dev) {
    if (!dev) return -EINVAL;
    driver_ops_lock();
    if (!device_is_registered(dev)) { driver_ops_unlock(); return -ENOENT; }
    int ret = device_unbind(dev);
    if (ret) { driver_ops_unlock(); return ret; }
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
#ifdef CONFIG_DRIVER_REGISTRY_RUST
    int remove = a20_driver_registry_remove((void **)&g_devices,
                                             &g_device_count, dev);
    if (remove < 0) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return -ENOENT;
    }
#else
    int found = 0;
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i] == dev) {
            g_devices[i] = g_devices[--g_device_count];
            found = 1;
            break;
        }
    }
    if (!found) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return -ENOENT;
    }
#endif
    spin_unlock_irqrestore(&g_driver_core_lock, flags);

    driver_ops_unlock();
    device_put(dev);
    return 0;
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
        device_t *held = device_get(dev);
        if (!held) return -ENODEV;
        int ret = device_unregister(dev);
        if (!ret && dev->bus && dev->bus->hotplug)
            dev->bus->hotplug(dev, BUS_EVENT_REMOVE);
        device_put(held);
        return ret;
    }
    return -EINVAL;
}

int bus_register(bus_type_t *bus) {
    if (!bus || !bus->name)
        return -EINVAL;
    driver_ops_lock();
    if (g_driver_shutdown_active) {
        driver_ops_unlock(); return -ESHUTDOWN;
    }
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
#ifdef CONFIG_DRIVER_REGISTRY_RUST
    int insert = a20_driver_registry_insert((void **)&g_buses,
        &g_bus_count, &g_bus_cap, bus, driver_registry_grow_bus);
    if (insert == -EEXIST) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return insert;
    }
    if (insert < 0) {
        kerr("[DRIVER] REGISTRY_CAPACITY_EXHAUSTED kind=bus capacity=%d error=%d\n",
             g_bus_cap, insert);
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return insert;
    }
#else
    for (int i = 0; i < g_bus_count; i++) {
        if (g_buses[i] == bus) {
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            driver_ops_unlock();
            return -EEXIST;
        }
    }
    if (g_bus_count >= g_bus_cap) {
        int new_cap = g_bus_cap * 2;
        bus_type_t **new_arr = driver_registry_realloc(
            g_buses, sizeof(*g_buses) * (size_t)new_cap,
            DRIVER_REGISTRY_TEST_BUS);
        if (!new_arr) {
            kerr("[DRIVER] REGISTRY_CAPACITY_EXHAUSTED kind=bus capacity=%d error=%d\n",
                 g_bus_cap, -ENOMEM);
            spin_unlock_irqrestore(&g_driver_core_lock, flags);
            driver_ops_unlock();
            return -ENOMEM;
        }
        g_buses = new_arr;
        g_bus_cap = new_cap;
    }
    g_buses[g_bus_count++] = bus;
#endif
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    driver_ops_unlock();
    kinfo("[DRIVER] registered bus '%s'\n", bus->name);
    return 0;
}

void bus_unregister(bus_type_t *bus) {
    if (!bus) return;
    driver_ops_lock();
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
#ifdef CONFIG_DRIVER_REGISTRY_RUST
    int remove = a20_driver_registry_remove((void **)&g_buses,
                                             &g_bus_count, bus);
    if (remove < 0) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return;
    }
#else
    int found = 0;
    for (int i = 0; i < g_bus_count; i++) {
        if (g_buses[i] == bus) {
            g_buses[i] = g_buses[--g_bus_count];
            found = 1;
            break;
        }
    }
    if (!found) {
        spin_unlock_irqrestore(&g_driver_core_lock, flags);
        driver_ops_unlock();
        return;
    }
#endif
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    driver_ops_unlock();
}

int bus_probe_device(device_t *dev) {
    if (!dev) return -EINVAL;
    driver_ops_lock();
    if (g_driver_shutdown_active) {
        driver_ops_unlock(); return -ESHUTDOWN;
    }
    if (dev->drv || device_is_stopping(dev)) {
        driver_ops_unlock();
        return -EBUSY;
    }
    int result = -ENODEV;
    for (int i = 0; i < g_driver_count; i++) {
        driver_t *drv = g_drivers[i];
        if (driver_matches_device(drv, dev)) {
            result = driver_probe_bound_device(drv, dev);
            if (result == 0) {
                driver_ops_unlock();
                return 0;
            }
            if (result == -EPROBE_DEFER || dev->drv) break;
        }
    }
    driver_ops_unlock();
    return result;
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

device_t *device_find_get_by_class(uint32_t class_type, int index) {
    if (index < 0) return NULL;
    class_device_t *cdev = class_device_get_by_type(class_type, (unsigned)index);
    if (!cdev) return NULL;
    device_t *dev = device_get(cdev->dev);
    class_device_put(cdev);
    return dev;
}

device_t *device_find_get_by_firmware(const fw_node_t *node) {
    if (!fw_node_valid(node)) return NULL;
    device_t *found = NULL;
    uint64_t flags = spin_lock_irqsave(&g_driver_core_lock);
    for (int i = 0; i < g_device_count; i++)
        if (fw_node_equal(&g_devices[i]->firmware_node, node)) {
            found = device_get(g_devices[i]); break;
        }
    spin_unlock_irqrestore(&g_driver_core_lock, flags);
    return found;
}
device_t *device_find_get_by_firmware_id(const char *id) {
    if (!id) return NULL;
    device_t *found = NULL;
    driver_ops_lock();
    for (int i = 0; i < g_device_count; i++)
        if (fw_node_match_id(&g_devices[i]->firmware_node, id) > 0 ||
            (g_devices[i]->software_id && !strcmp(g_devices[i]->software_id, id))) {
            found = device_get(g_devices[i]); break;
        }
    driver_ops_unlock();
    return found;
}
device_t *device_find_get_by_mmio(uint64_t physical) {
    device_t *found = NULL;
    driver_ops_lock();
    for (int i = 0; i < g_device_count; i++) {
        resource_t *resource = device_get_resource(g_devices[i], RES_MMIO, 0);
        if (resource && resource->start == physical) { found = device_get(g_devices[i]); break; }
    }
    driver_ops_unlock();
    return found;
}
int driver_device_count(void) { return g_device_count; }
void driver_lifecycle_begin(void) { driver_ops_lock(); }
void driver_lifecycle_end(void) { driver_ops_unlock(); }
int driver_lifecycle_try_begin(void) {
    bool boot = !proc_current();
    unsigned cpu = cpu_current_id();
    /* Progress must not re-enter its own partially mutated boot graph. Task
     * mutex_trylock is likewise non-recursive, unlike blocking mutex_lock. */
    if ((boot && boot_ops_depth[cpu]) || !mutex_trylock(&g_driver_core_ops))
        return -EAGAIN;
    if (boot) boot_ops_depth[cpu] = 1;
    return 0;
}

void driver_discovery_begin(void) {
    driver_ops_lock();
    discovery_depth++;
}
void driver_discovery_end(void) {
    if (!discovery_depth) panic("driver discovery transaction underflow");
    discovery_depth--;
    driver_ops_unlock();
}
int driver_retry_deferred(void) {
    int total = 0, progress;
    static int retrying;
    driver_ops_lock();
    if (discovery_depth || retrying) { driver_ops_unlock(); return 0; }
    retrying = 1;
    /* Normally each pass binds at least one of this finite device set. A
     * callback may mutate that set; bound retries even in that case. */
    unsigned passes = (unsigned)g_device_count + 1;
    do {
        progress = 0;
        for (int i = 0; i < g_device_count; i++) {
            device_t *dev = g_devices[i];
            if (!dev->drv && dev->probe_error == -EPROBE_DEFER && !bus_probe_device(dev)) {
                progress++; total++;
            }
        }
    } while (progress && --passes);
    retrying = 0;
    driver_ops_unlock();
    return total;
}

void driver_probe_all(void) {
    int probed = 0;
    driver_ops_lock();
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv) continue;
        if (bus_probe_device(dev) == 0)
            probed++;
    }
    probed += driver_retry_deferred();
    driver_ops_unlock();
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
        /* Pin the class device for the callback.  class_device_unpublish()
         * waits for the matching call_end() and runs before drv->remove(),
         * so a driver cannot have its private state freed under a poll that
         * has already entered this window. */
        class_device_t *cdev = class_device_ref_for_device(dev);
        if (!cdev)
            continue;
        if (class_device_call_begin(cdev) == 0) {
            /* Even reading drv->class_type requires this guard: a module's
             * driver object may have been freed after mirror creation. */
            driver_t *drv = dev->drv;
            if (drv && drv->class_type == class_type && drv->progress)
                drv->progress(dev);
            class_device_call_end(cdev);
        }
        class_device_put(cdev);
    }
}

static int driver_core_shutdown_supported(void)
{
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (dev->drv && !dev->drv->remove) {
            kinfo("[SHUTDOWN] driver '%s' has no fallible stop/remove protocol\n",
                  dev->drv->name ? dev->drv->name : "?");
            return -EOPNOTSUPP;
        }
    }
    return 0;
}

int driver_core_kexec_prepare(void)
{
    driver_ops_lock();
    int rc = driver_core_shutdown_supported();
    driver_ops_unlock();
    return rc;
}

int driver_core_shutdown_all(void)
{
    driver_ops_lock();
    int rc = driver_core_shutdown_supported();
    if (rc) {
        driver_ops_unlock();
        return rc;
    }
    g_driver_shutdown_active = 1;
    for (;;) {
        device_t *dev = NULL;
        /* Unbind may retire child declarations through devres. Rescan the
         * current registry instead of retaining an index across callbacks. */
        for (int i = g_device_count - 1; i >= 0; i--) {
            if (g_devices[i]->drv) { dev = g_devices[i]; break; }
        }
        if (!dev) break;
        device_get(dev);
        /* Reuse prepare/consumer-first/stop/quiesce/class-drain/remove.
         * Its error path retains the binding, DMA storage and dependencies. */
        rc = device_unbind(dev);
        device_put(dev);
        if (rc) break;
    }
    driver_ops_unlock();
    return rc;
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

/* Format the registered devices for the MCU shell / diagnostics. */
int driver_core_list_devices(char *buf, size_t sz)
{
    if (!buf)
        return -EINVAL;
    size_t off = 0;
    for (int i = 0; i < g_device_count; i++) {
        device_t *dev = g_devices[i];
        if (!dev || !dev->name)
            continue;
        int n = snprintf(buf + off, sz > off ? sz - off : 0,
                         "%s state=%d drv=%s\n", dev->name, dev->state,
                         dev->drv && dev->drv->name ? dev->drv->name : "-");
        if (n < 0)
            break;
        off += (size_t)n;
        if (off >= sz)
            break;
    }
    return (int)off;
}
