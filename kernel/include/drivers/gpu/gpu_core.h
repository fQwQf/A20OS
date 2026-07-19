#ifndef _GPU_CORE_H
#define _GPU_CORE_H

#include "drivers/core/driver_core.h"

/* The first display device that completes probe owns /dev/fb0. */
int gpu_device_register(device_t *dev);
void gpu_device_unregister(device_t *dev);
device_t *gpu_device_get_default(void);

#endif
