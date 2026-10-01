#ifndef _GPU_CORE_H
#define _GPU_CORE_H

#include "drivers/core/driver_core.h"

/* A 3D-capable display device is preferred as the /dev/fb0 and DRM backend;
 * see gpu_device_register() in gpu_core.c for the promotion rule. */
int gpu_device_register(device_t *dev);
void gpu_device_unregister(device_t *dev);
device_t *gpu_device_get_default(void);

#endif
