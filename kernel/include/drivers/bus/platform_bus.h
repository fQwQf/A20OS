#ifndef _DRIVERS_BUS_PLATFORM_BUS_H
#define _DRIVERS_BUS_PLATFORM_BUS_H

#include "drivers/core/driver_core.h"

extern bus_type_t platform_bus;

typedef struct platform_device {
    device_t dev;
} platform_device_t;

int platform_device_register(platform_device_t *pdev);
int platform_device_unregister(platform_device_t *pdev);

/*
 * Read the first resolved IRQ resource. @dev must be on platform_bus.
 * Firmware IRQ specifiers are resolved by their controller domain before
 * probe; resource addresses carry driver IRQ identifiers, not raw DT cells.
 *
 *   >= 0            the IRQ line, usable with request_irq();
 *   -ENODEV         the board published no RES_IRQ for this device.  This is
 *                   the ordinary case for a device meant to be polled, and it
 *                   is not an error: the caller keeps its polling path;
 *   -EINVAL         not a platform device, or a RES_IRQ whose end != start --
 *                   a range is not a line, and request_irq() would interpret
 *                   the whole range as one number;
 *   -ERANGE         a line outside the fixed 256-entry IRQ registry, which
 *                   request_irq() would also reject.  Reported here so the
 *                   fallback decision is made once, in the driver, instead of
 *                   after a register call whose only signal is -EINVAL.
 *
 * Only a driver with an explicit polling implementation may treat an absent
 * line as optional. Invalid descriptions must not become guessed IRQs.
 */
#define PLATFORM_IRQ_MAX_LINES 256U

int platform_device_irq(device_t *dev);

#endif
