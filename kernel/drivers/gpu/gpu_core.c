#include "drivers/gpu/gpu_core.h"
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
    /* Promotion only: a 3D-capable newcomer may displace a 2D-only incumbent,
     * but two equally capable devices never trade the slot back and forth, so
     * probe order cannot churn the DRM binding. */
    if (!g_default_gpu ||
        (dev_3d && !gpu_dev_can_3d(g_default_gpu))) {
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
    if (g_default_gpu == dev) {
        g_default_gpu = NULL;
        printf("[GPU] gpu_device_unregister: default_gpu=%p cleared\n", dev);
    }
    spin_unlock_irqrestore(&g_gpu_lock, flags);
}

device_t *gpu_device_get_default(void) {
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    device_t *dev = g_default_gpu;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    return dev;
}
