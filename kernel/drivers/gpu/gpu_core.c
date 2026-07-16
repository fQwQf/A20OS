#include "drivers/gpu/gpu_core.h"
#include "drivers/core/driver_class.h"

static device_t *g_default_gpu;

int gpu_device_register(device_t *dev) {
    if (!dev || !dev->drv || dev->drv->class_type != DEV_CLASS_DISPLAY ||
        !dev->drv->class_ops)
        return -1;

    if (!g_default_gpu)
        g_default_gpu = dev;
    return 0;
}

device_t *gpu_device_get_default(void) {
    return g_default_gpu;
}
