/*
 * virtio-input kernel probe — drvmod module.
 *
 * The kernel placement of the dual-placement virtio-input driver now runs
 * as a loadable module (docs/hybrid-kernel/04-dual-placement.md).  It uses
 * the same shared protocol source as the user driver (user/svc/uinputd.c)
 * through the DRV_ENV_DRVMOD placement of drv_env.h; the probe is
 * deliberately read-only (config-space queries only), because full device
 * init (status transitions, virtqueues) is destructive and single-owner.
 *
 * The matched platform device identity carries the virtio-mmio slot base
 * address, so one module binary serves every board with a user-owned
 * virtio-input slot (riscv64 slot base 0x10006000, aarch64 slot 5 at
 * 0x0A000A00).  The driver core marks the slot user-owned, and this
 * driver's read_only_probe flag is what allows it to bind at all.
 */

#define DRV_ENV_DRVMOD 1
#include "drvmod/drvmod.h"
#include <stdint.h>

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_INPUT, "virtio-input-probe", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO,
                      0, 2,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_FIXED,
                                           0x10006000UL, 0),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_FIXED,
                                           0x0A000A00UL, 0));
#include "drivers/dual/virtio_input.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "core/errno.h"

/* Only transport identity and the input config latch are queried. AArch64
 * virt slots are 0x200 bytes; a fixed 4KiB window spans neighboring devices. */
#define VINPUT_PROBE_SIZE (VMMIO_CONFIG + VINPUT_CFG_DATA + VINPUT_CFG_DATA_MAX)

static int vinput_probe(device_t *dev)
{
    if (!dev)
        return -ENODEV;
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res || res->end < res->start || res->end > UINTPTR_MAX ||
        (res->start & 3u) || res->end - res->start < VINPUT_PROBE_SIZE - 1u)
        return -ENODEV;
    uint64_t base = drv_mmio_map(res->start, VINPUT_PROBE_SIZE, 3);
    if (!base)
        return -EIO;
    vmmio_probe_t p;
    int found = 0;
    if (vmmio_probe(base, &p) == 0 &&
        p.device_id == VIRTIO_INPUT_DEVICE_ID) {
        char name[64];
        uint32_t n = vinput_cfg_string(base, VIRTIO_INPUT_CFG_ID_NAME,
                                       name, sizeof(name));
        drv_log("[UINPUT] kernel-placement probe: id=%u version=%u name=%s\n",
                p.device_id, p.version, n ? name : "?");
        found = 1;
    }
    drv_mmio_unmap(base, VINPUT_PROBE_SIZE);
    return found ? 0 : -ENODEV;
}

static int vinput_probe_remove(device_t *dev)
{
    /* Probe unmaps synchronously and never owns queues, IRQs or workers.
     * Publish this empty retirement contract so terminal shutdown can drain
     * the diagnostic binding without inventing a destructive device reset. */
    (void)dev;
    return 0;
}

static const firmware_device_id_t firmware_ids[] = {
    {.id = "virtio,mmio"}, {0}
};
static driver_t vinput_probe_driver = {
    .name = "virtio-input-probe",
    .firmware_ids = firmware_ids,
    .bus = &platform_bus,
    .read_only_probe = 1,   /* user service uinputd owns the slot */
    .probe = vinput_probe,
    .remove = vinput_probe_remove,
    .class_type = DEV_CLASS_NONE,
};

uintptr_t DriverEntry(void)
{
    int r = drv_driver_register(&vinput_probe_driver);
    drv_log("[UINPUT] kernel-placement driver registered in core: %d\n", r);
    return r == 0 ? 0 : 1;
}
