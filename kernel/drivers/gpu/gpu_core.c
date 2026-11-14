#include "drivers/gpu/gpu_core.h"
#include "drivers/gpu/gpu_select.h"
#include "drivers/core/driver_class.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"

static device_t *g_default_gpu; /* owns one metadata reference */
static bool g_default_3d;
static spinlock_t g_gpu_lock = SPINLOCK_INIT;

/* Only inspect under probe ownership or a live class call. This describes
 * the complete virgl transport consumed by DRM, not arbitrary vendor queues
 * or the features actually negotiated with the device. */
static bool gpu_ops_can_3d(const gpu_dev_ops_t *ops) {
    return ops && ops->get_capset && ops->capset_info &&
           ops->resource_attach_backing && ops->ctx_create &&
           ops->ctx_destroy && ops->resource_create_3d &&
           ops->resource_unref && ops->submit_3d;
}

int gpu_device_register(device_t *dev) {
    driver_lifecycle_begin();
    int result = -EINVAL;
    device_t *old = NULL;
    if (!dev) goto out;
    if (!device_is_registered(dev) || device_is_stopping(dev)) { result = -ENODEV; goto out; }
    if (!dev->drv || dev->drv->class_type != DEV_CLASS_DISPLAY || !dev->drv->class_ops)
        goto out;
    bool capable = gpu_ops_can_3d(dev->drv->class_ops);
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    bool take = g_default_gpu == dev ||
                gpu_select_should_take(g_default_gpu != NULL, g_default_3d, capable);
    if (take && g_default_gpu != dev) {
        device_t *owned = device_get(dev);
        if (!owned) { spin_unlock_irqrestore(&g_gpu_lock, flags); result = -ENODEV; goto out; }
        old = g_default_gpu;
        g_default_gpu = owned;
    }
    if (take) g_default_3d = capable;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    if (take)
        printf("[GPU] gpu_device_register: default_gpu=%p class_type=%d 3d=%d\n",
               dev, DEV_CLASS_DISPLAY, capable);
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
    if (old) { g_default_gpu = NULL; g_default_3d = false; }
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    if (!old) { driver_lifecycle_end(); return; }

    /* Selection is policy over online DISPLAY publications, not another
     * publication registry. Lifecycle stabilizes enumeration; class calls
     * protect operation tables. Do not stop at an offline class rank or an
     * arbitrary sixteen-device limit. */
    device_t *best = NULL;
    bool best_3d = false;
    for (int index = 0;; index++) {
        device_t *candidate = device_find_get_by_class(DEV_CLASS_DISPLAY, index);
        if (!candidate) break;
        struct gpu_device_access access;
        class_device_t *publication = class_device_ref_for_device(candidate);
        int admitted = candidate == dev ? -ENODEV : gpu_device_access_begin(publication, &access);
        class_device_put(publication);
        if (!admitted) {
            bool capable = gpu_ops_can_3d(access.ops);
            if (gpu_select_better(best != NULL, best_3d, capable)) {
                device_put(best);
                best = candidate; /* Transfer the enumerator's owning reference. */
                candidate = NULL;
                best_3d = capable;
            }
            gpu_device_access_end(&access);
        }
        device_put(candidate);
        if (best_3d) break;
    }
    flags = spin_lock_irqsave(&g_gpu_lock);
    if (!g_default_gpu) {
        g_default_gpu = best;
        g_default_3d = best_3d;
        best = NULL;
    }
    device_t *selected = g_default_gpu;
    bool selected_3d = g_default_3d;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    printf("[GPU] gpu_device_unregister: default_gpu=%p re-elected (%s)\n",
           selected, selected_3d ? "3D-capable" : selected ? "2D-only" : "none");
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

device_t *gpu_device_get_default(void) {
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    device_t *dev = g_default_gpu;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    return dev;
}
