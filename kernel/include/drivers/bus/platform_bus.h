#ifndef _DRIVERS_BUS_PLATFORM_BUS_H
#define _DRIVERS_BUS_PLATFORM_BUS_H

#include "drivers/core/driver_core.h"

extern bus_type_t platform_bus;

typedef struct platform_device {
    device_t dev;
    device_id_t id;
} platform_device_t;

int platform_device_register(platform_device_t *pdev);
void platform_device_unregister(platform_device_t *pdev);

#endif
