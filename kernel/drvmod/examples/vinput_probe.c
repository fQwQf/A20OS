/*
 * virtio-input kernel probe — drvmod module.
 *
 * Migrated from kernel/drivers/input/virtio_input_kprobe.c (removed): the kernel
 * placement of the dual-placement virtio-input driver now runs as a
 * loadable module (docs/hybrid-kernel/04-dual-placement.md).  It uses
 * the same shared protocol source as the user driver (user/svc/uinputd.c)
 * through the DRV_ENV_DRVMOD placement of drv_env.h; the probe is
 * deliberately read-only (config-space queries only), because full device
 * init (status transitions, virtqueues) is destructive and single-owner.
 *
 * The matched device identity carries the virtio-mmio slot base address
 * (dev->vendor), so one module binary serves every board with a virtio-mmio
 * bus (riscv64 slot base 0x10006000, aarch64 slot 5 at 0x0A000A00).
 */

#define DRV_ENV_DRVMOD 1
#include "drvmod/drvmod.h"
#include "drivers/dual/virtio_input.h"

#define VINPUT_DUAL_SIZE 0x1000ULL

static int vinput_probe(drv_device_t *dev)
{
    if (!dev)
        return -1;
    uint64_t base = drv_mmio_map(dev->vendor, VINPUT_DUAL_SIZE, 3);
    if (!base)
        return -1;
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
    drv_mmio_unmap(base, VINPUT_DUAL_SIZE);
    return found ? 0 : -1;
}

/* Pins the module while bound (the probe is stateless). */
static drv_driver_t g_modinfo;

uintptr_t DriverEntry(drv_driver_t **out)
{
    g_modinfo.name = "vinput-probe";
    g_modinfo.match_count = 2;
    g_modinfo.match[0].bus = 3;
    g_modinfo.match[0].vendor = 0x10006000UL;   /* riscv64/loongarch64 slot 5 */
    g_modinfo.match[0].device = 0;
    g_modinfo.match[1].bus = 3;
    g_modinfo.match[1].vendor = 0x0A000A00UL;   /* aarch64 slot 5 (0x200 spacing) */
    g_modinfo.match[1].device = 0;
    g_modinfo.probe = vinput_probe;
    g_modinfo.remove = NULL;
    if (out)
        *out = &g_modinfo;
    return 0;
}
