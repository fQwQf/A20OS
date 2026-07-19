#include "drivers/gpu/gpu_core.h"
#include "drivers/core/driver_class.h"
#include "core/errno.h"
#include "core/lock.h"

static device_t *g_default_gpu;
static spinlock_t g_gpu_lock = SPINLOCK_INIT;

int gpu_device_register(device_t *dev) {
    if (!dev || !dev->drv || dev->drv->class_type != DEV_CLASS_DISPLAY ||
        !dev->drv->class_ops)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    if (!g_default_gpu)
        g_default_gpu = dev;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    return 0;
}

void gpu_device_unregister(device_t *dev) {
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    if (g_default_gpu == dev)
        g_default_gpu = NULL;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
}

device_t *gpu_device_get_default(void) {
    uint64_t flags = spin_lock_irqsave(&g_gpu_lock);
    device_t *dev = g_default_gpu;
    spin_unlock_irqrestore(&g_gpu_lock, flags);
    return dev;
}
