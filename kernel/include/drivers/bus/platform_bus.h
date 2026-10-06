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

/*
 * PLATFORM_IRQ_RESOURCE_CHANNEL: the platform bus already carries every
 * resource kind in device_t.res[] -- RES_MMIO, RES_IRQ, RES_DMA, RES_MEM,
 * RES_IOPORT -- and both a board's fixed device table and the RISC-V device
 * tree enumerator (kernel/arch/riscv64/platform/fdt_dev.c) fill them in.  What
 * was missing is a driver-facing way to ask for the line with defined failure
 * semantics, so every driver had to re-derive "absent" from a NULL resource
 * pointer and could not tell "the board published no line" (keep polling) from
 * "the board published a broken line" (a real error).
 *
 * This is that accessor.  @dev must be on platform_bus.
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
 * Backward compatibility is total: a device with no RES_IRQ gets -ENODEV, and
 * a driver that has never heard of this function keeps working unchanged.
 */
#define PLATFORM_IRQ_MAX_LINES 256U

int platform_device_irq(device_t *dev);

#endif
