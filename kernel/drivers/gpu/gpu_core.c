#include "drivers/gpu/gpu_core.h"
#include "drivers/gpu/gpu_select.h"
#include "drivers/core/driver_class.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"

static device_t *g_default_gpu;
static spinlock_t g_gpu_lock = SPINLOCK_INIT;

/*
 * 3D-capable == it carries the whole virgl command set DRM dispatches.  DRM
 * NULL-checks exactly these entry points per ioctl and answers -ENODEV when
 * one is missing, so an empty 3D vtable silently strips the whole 3D path
 * while still advertising capabilities.  get_features is excluded on purpose:
 * a 2D-only driver can implement it and hardcode out_3d = 0, so it does not
 * discriminate.  Requiring the full set, not one probe like get_capset, keeps
 * a half-implemented 3D driver from winning over a complete one.
 */
static bool gpu_dev_can_3d(const device_t *dev) {
    if (!dev || !dev->drv)
        return false;
    const gpu_dev_ops_t *ops = dev->drv->class_ops;
    return ops && ops->get_capset && ops->capset_info &&
           ops->resource_attach_backing && ops->ctx_create &&
           ops->ctx_destroy && ops->resource_create_3d &&
           ops->resource_unref && ops->submit_3d;
}

int gpu_device_register(device_t *dev) {
    if (!dev || !dev->drv || dev->drv->class_type != DEV_CLASS_DISPLAY ||
        !dev->drv->class_ops)
        return -EINVAL;

    bool dev_3d = gpu_dev_can_3d(dev);
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    /* Promotion only, per gpu_select_should_take(): a 3D-capable newcomer may
     * displace a 2D-only incumbent, but two equally capable devices never trade
     * the slot back and forth, so probe order cannot churn the DRM binding. */
    if (gpu_select_should_take(g_default_gpu != NULL,
                               g_default_gpu && gpu_dev_can_3d(g_default_gpu),
                               dev_3d)) {
        const char *why = !g_default_gpu
                              ? "first display device"
                              : (dev_3d ? "3D-capable displaces 2D-only incumbent"
                                        : "no incumbent, 2D-only");
        g_default_gpu = dev;
        printf("[GPU] gpu_device_register: default_gpu=%p class_type=%d 3d=%d (%s)\n",
               dev, dev->drv->class_type, dev_3d, why);
    }
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    return 0;
}

void gpu_device_unregister(device_t *dev) {
    /* Re-test identity under the lock: a newer 3D-capable device may already
     * have replaced this one in the slot, and clearing it then would leave DRM
     * with no backend at all. */
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    bool was_default = (g_default_gpu == dev);
    if (was_default)
        g_default_gpu = NULL;
    spin_unlock_irqrestore(&g_gpu_lock, flags);

    if (!was_default)
        return;

    /* Re-elect from the devices that are still online.  Clearing the slot
     * alone left DRM with no backend at all whenever the incumbent went away
     * and another display device was still present, so every ioctl answered
     * -ENODEV until something forced a re-probe.  The promotion rule is
     * deliberately the same one register() uses, so the outcome does not depend
     * on which function noticed the departure.
     *
     * The winner is a bare device_t*, exactly as the slot has always held:
     * class_device_get_by_type() only returns online devices, and a published
     * device outlives this call, so the pointer is as stable here as it is in
     * gpu_device_register().  Taking a class reference instead would leak it --
     * nothing drops the slot's reference on a later unregister. */
    device_t *best = NULL;
    bool best_3d = false;
    for (unsigned i = 0; i < 16; i++) {
        class_device_t *cd = class_device_get_by_type(DEV_CLASS_DISPLAY, i);
        if (!cd)
            break;
        /* Skip ourselves: unpublish() marks the departing device offline before
         * remove() runs, so it is normally already excluded, but not when a
         * driver tears itself down directly. */
        if (cd->dev != dev) {
            bool cand_3d = gpu_dev_can_3d(cd->dev);
            if (gpu_select_better(best != NULL, best_3d, cand_3d)) {
                best = cd->dev;
                best_3d = cand_3d;
            }
        }
        bool stop = best_3d;   /* a 3D winner cannot be improved on */
        class_device_put(cd);
        if (stop)
            break;
    }

    flags = spin_lock_irqsave(&g_gpu_lock);
    /* Only claim an empty slot, and never displace one filled while we were
     * enumerating: a concurrent register() has already made its choice. */
    if (!g_default_gpu) {
        g_default_gpu = best;
        if (best)
            printf("[GPU] gpu_device_unregister: default_gpu=%p re-elected (%s)\n",
                   best, best_3d ? "3D-capable" : "2D-only");
        else
            printf("[GPU] gpu_device_unregister: no display device remains\n");
    }
    spin_unlock_irqrestore(&g_gpu_lock, flags);
}

device_t *gpu_device_get_default(void) {
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    device_t *dev = g_default_gpu;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    return dev;
}
