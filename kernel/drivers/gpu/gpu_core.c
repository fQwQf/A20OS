#include "drivers/gpu/gpu_core.h"
#include "drivers/gpu/gpu_select.h"
#include "drivers/core/driver_class.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "mm/vmo.h"
#include "mm/mapping_owner.h"
#include "mm/slab.h"
#include "drivers/core/protocol.h"

struct gpu_physical_mapping_ops {
    struct mm_mapping_owner *owner;
};
struct gpu_physical_mapping {
    struct mm_mapping_owner owner; /* first: kernel-owned finalizer */
    struct gpu_physical_mapping_ops ops;
    device_t *device;
    spinlock_t lock;
    bool closing;
};

static void gpu_physical_mapping_release(struct mm_mapping_owner *owner) {
    struct gpu_physical_mapping *mapping = (void *)owner;
    device_put(mapping->device);
    kfree(mapping);
}

static void gpu_physical_mapping_drop(void *context) {
    struct gpu_physical_mapping *mapping = context;
    mm_mapping_owner_put(&mapping->owner);
}

static int gpu_physical_mapping_prepare(void *context) {
    struct gpu_physical_mapping *mapping = context;
    uint64_t flags = spin_lock_irqsave(&mapping->lock);
    mapping->closing = true;
    /* The devres owner is the one reference which is not a borrowed view.
     * Never enter the legacy class drain while a VMA needs this binding. */
    int ret = refcount_read(&mapping->owner.references) > 1 ? -EBUSY : 0;
    spin_unlock_irqrestore(&mapping->lock, flags);
    return ret;
}

int gpu_device_physical_mapping_init(device_t *device) {
    if (!device || !device->drv || device->drv->class_type != DEV_CLASS_DISPLAY)
        return -EINVAL;
    int ret = device_probe_use_remove(device);
    if (ret) return ret;
    struct gpu_physical_mapping *mapping = kcalloc(1, sizeof(*mapping));
    if (!mapping) return -ENOMEM;
    mapping->device = device_get(device);
    if (!mapping->device) { kfree(mapping); return -ENODEV; }
    spin_init(&mapping->lock);
    mm_mapping_owner_init(&mapping->owner, gpu_physical_mapping_release);
    mapping->ops.owner = &mapping->owner;
    ret = device_add_resource_action(device, gpu_physical_mapping_drop, mapping);
    if (ret) { gpu_physical_mapping_drop(mapping); return ret; }
    ret = device_add_remove_prepare(device, gpu_physical_mapping_prepare, mapping);
    if (ret) return ret; /* Managed probe cleanup owns the previous action. */
    const struct driver_protocol_descriptor protocol = {
        .info = {DRIVER_PROTOCOL_DISPLAY_MAPPING, 1, sizeof(mapping->ops), 0},
        .ops = &mapping->ops,
    };
    return device_add_protocol(device, &protocol);
}

int gpu_device_physical_mapping_get(const struct gpu_device_access *access,
                                     struct mm_mapping_owner **out) {
    if (!out) return -EINVAL;
    *out = NULL;
    if (!access || !access->publication || !access->device || !access->ops)
        return -ENODEV;
    const struct driver_protocol_requirement required = {
        DRIVER_PROTOCOL_DISPLAY_MAPPING, 1, sizeof(struct gpu_physical_mapping_ops), 0,
    };
    struct driver_protocol_access protocol;
    int ret = driver_protocol_begin(access->publication, &required, &protocol);
    if (ret) return ret;
    const struct gpu_physical_mapping_ops *ops = protocol.ops;
    if (!ops->owner || protocol.device != access->device) {
        driver_protocol_end(&protocol);
        return -EIO;
    }
    struct gpu_physical_mapping *mapping = (void *)ops->owner;
    uint64_t flags = spin_lock_irqsave(&mapping->lock);
    ret = mapping->closing ? -ENODEV : 0;
    if (!ret) { mm_mapping_owner_get(&mapping->owner); *out = &mapping->owner; }
    spin_unlock_irqrestore(&mapping->lock, flags);
    driver_protocol_end(&protocol);
    return ret;
}

static device_t *g_default_gpu; /* owns one metadata reference */
static unsigned g_default_rank;
static spinlock_t g_gpu_lock = SPINLOCK_INIT;

/* Serialized binding ownership or an active class access protects state/code.
 * Keep provider execution outside the selection lock. The result is native
 * role discovery; it does not infer an implementation from virgl callbacks. */
static int gpu_capabilities_read(device_t *device, const gpu_dev_ops_t *ops,
                                  struct gpu_capabilities *out)
{
    *out = (struct gpu_capabilities){0};
    if (!ops->get_capabilities) return -EOPNOTSUPP;
    struct gpu_capabilities capabilities = {0};
    int result = ops->get_capabilities(device, &capabilities);
    if (result) return result < 0 ? result : -EIO;
    if ((capabilities.flags & ~GPU_CAP_KNOWN) ||
        ((capabilities.flags & GPU_CAP_SCANOUT) && !ops->get_info) ||
        ((capabilities.flags & GPU_CAP_BOOT_SCANOUT) && !(capabilities.flags & GPU_CAP_SCANOUT)) ||
        ((capabilities.flags & GPU_CAP_RAM_SCANOUT) &&
         (!(capabilities.flags & GPU_CAP_SCANOUT) || !ops->get_scanout)))
        return -EINVAL;
    *out = capabilities;
    return 0;
}

int gpu_device_capabilities_get(const struct gpu_device_access *access,
                                 struct gpu_capabilities *out)
{
    if (!out) return -EINVAL;
    *out = (struct gpu_capabilities){0};
    if (!access || !access->publication || !access->device || !access->ops)
        return -ENODEV;
    return gpu_capabilities_read(access->device, access->ops, out);
}

int gpu_device_register(device_t *dev) {
    driver_lifecycle_begin();
    int result = -EINVAL;
    device_t *old = NULL;
    if (!dev) goto out;
    if (!device_is_registered(dev) || device_is_stopping(dev)) { result = -ENODEV; goto out; }
    if (!dev->drv || dev->drv->class_type != DEV_CLASS_DISPLAY || !dev->drv->class_ops)
        goto out;
    struct gpu_capabilities capabilities;
    result = gpu_capabilities_read(dev, dev->drv->class_ops, &capabilities);
    if (result) goto out;
    if (!(capabilities.flags & GPU_CAP_SCANOUT)) goto out;
    unsigned rank = gpu_select_rank(capabilities.flags & GPU_CAP_RENDER,
                                    capabilities.flags & GPU_CAP_BOOT_SCANOUT);
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    bool take = g_default_gpu == dev ||
                gpu_select_should_take(g_default_gpu != NULL, g_default_rank, rank);
    if (take && g_default_gpu != dev) {
        device_t *owned = device_get(dev);
        if (!owned) { spin_unlock_irqrestore(&g_gpu_lock, flags); result = -ENODEV; goto out; }
        old = g_default_gpu;
        g_default_gpu = owned;
    }
    if (take) g_default_rank = rank;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    if (take)
        printf("[GPU] gpu_device_register: default_gpu=%p class_type=%d priority=%u\n",
               dev, DEV_CLASS_DISPLAY, rank);
    result = 0;
out:
    device_put(old); /* Publisher disposal is never under g_gpu_lock. */
    driver_lifecycle_end();
    return result;
}

void gpu_device_unregister(device_t *dev) {
    driver_lifecycle_begin();
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    device_t *old = g_default_gpu == dev ? g_default_gpu : NULL;
    if (old) { g_default_gpu = NULL; g_default_rank = 0; }
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    if (!old) { driver_lifecycle_end(); return; }

    /* Selection is policy over online DISPLAY publications, not another
     * publication registry. Lifecycle stabilizes enumeration; class calls
     * protect operation tables. Do not stop at an offline class rank or an
     * arbitrary sixteen-device limit. */
    device_t *best = NULL;
    unsigned best_rank = 0;
    for (int index = 0;; index++) {
        device_t *candidate = device_find_get_by_class(DEV_CLASS_DISPLAY, index);
        if (!candidate) break;
        struct gpu_device_access access;
        class_device_t *publication = class_device_ref_for_device(candidate);
        int admitted = candidate == dev ? -ENODEV : gpu_device_access_begin(publication, &access);
        class_device_put(publication);
        if (!admitted) {
            struct gpu_capabilities capabilities;
            int queried = gpu_device_capabilities_get(&access, &capabilities);
            bool eligible = !queried && (capabilities.flags & GPU_CAP_SCANOUT);
            unsigned rank = gpu_select_rank(capabilities.flags & GPU_CAP_RENDER,
                                            capabilities.flags & GPU_CAP_BOOT_SCANOUT);
            if (eligible && gpu_select_better(best != NULL, best_rank, rank)) {
                device_put(best);
                best = candidate; /* Transfer the enumerator's owning reference. */
                candidate = NULL;
                best_rank = rank;
            }
            gpu_device_access_end(&access);
        }
        device_put(candidate);
        if (best_rank == GPU_SELECT_MAX_RANK) break;
    }
    flags = spin_lock_irqsave(&g_gpu_lock);
    if (!g_default_gpu) {
        g_default_gpu = best;
        g_default_rank = best_rank;
        best = NULL;
    }
    device_t *selected = g_default_gpu;
    unsigned selected_rank = g_default_rank;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    printf("[GPU] gpu_device_unregister: default_gpu=%p re-elected (%s)\n",
           selected, selected_rank >= 2 ? "render-capable" : selected ?
           (selected_rank ? "managed scanout" : "firmware scanout") : "none");
    device_put(best);
    device_put(old);
    driver_lifecycle_end();
}

class_device_t *gpu_device_default_get(void) {
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    device_t *dev = device_get(g_default_gpu);
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    class_device_t *publication = class_device_ref_for_device(dev);
    device_put(dev);
    return publication;
}

int gpu_device_access_begin(class_device_t *publication, struct gpu_device_access *access) {
    if (!access) return -EINVAL;
    *access = (struct gpu_device_access){0};
    class_device_get(publication);
    const void *ops;
    int result = class_device_ops_begin(publication, DEV_CLASS_DISPLAY, &ops);
    if (result) { class_device_put(publication); return result; }
    *access = (struct gpu_device_access){publication, publication->dev, ops};
    return 0;
}

int gpu_device_default_begin(struct gpu_device_access *access) {
    if (!access) return -EINVAL;
    class_device_t *publication = gpu_device_default_get();
    int result = gpu_device_access_begin(publication, access);
    class_device_put(publication);
    return result;
}

void gpu_device_access_end(struct gpu_device_access *access) {
    if (!access) return;
    class_device_t *publication = access->publication;
    *access = (struct gpu_device_access){0};
    class_device_call_end(publication);
    class_device_put(publication);
}

void gpu_scanout_put(struct gpu_scanout *scanout) {
    if (!scanout) return;
    struct vmo *memory = scanout->memory;
    *scanout = (struct gpu_scanout){0};
    vmo_release(memory);
}

int gpu_device_scanout_get(const struct gpu_device_access *access, struct gpu_scanout *out) {
    if (!out) return -EINVAL;
    *out = (struct gpu_scanout){0};
    if (!access || !access->publication || !access->device || !access->ops) return -ENODEV;
    if (!access->ops->get_scanout) return -EOPNOTSUPP;
    struct gpu_scanout snapshot = {0};
    int ret = access->ops->get_scanout(access->device, &snapshot);
    uint64_t physical;
    size_t extent;
    if (!ret && (vmo_contiguous_info(snapshot.memory, &physical, &extent) ||
        !snapshot.size || snapshot.size > SIZE_MAX - (PAGE_SIZE - 1) ||
        (snapshot.offset & (PAGE_SIZE - 1)) || snapshot.offset >= extent ||
        ((snapshot.size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1)) > extent - snapshot.offset))
        ret = -EINVAL;
    if (ret) { gpu_scanout_put(&snapshot); return ret < 0 ? ret : -EIO; }
    *out = snapshot;
    return 0;
}

device_t *gpu_device_get_default(void) {
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    device_t *dev = g_default_gpu;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    return dev;
}
