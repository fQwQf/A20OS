/*
 * virtio_input_kprobe — kernel placement of the dual-placement
 * virtio-input driver (docs/hybrid-kernel/04-dual-placement.md).
 *
 * Same shared protocol source as the user driver (user/svc/uinputd.c);
 * this shell is deliberately read-only: full device init (status
 * transitions, virtqueues) is destructive and single-owner, so it
 * belongs to whichever placement owns the device at runtime.  The
 * point of this probe is to prove the shared protocol runs in kernel
 * placement and yields the same device identity as the user placement.
 */
#define DRV_ENV_KERNEL 1
#include "drivers/dual/virtio_input.h"
#include "drivers/input/virtio_input_kprobe.h"
#include "core/klog.h"

/* qemu-virt-riscv64 virtio-mmio bus.5 (slot base 0x10001000 + 5*0x1000);
 * whitelisted user-owned so the in-tree virtio_input driver never binds. */
#define VINPUT_DUAL_PHYS 0x10006000ULL
#define VINPUT_DUAL_SIZE 0x1000ULL

void virtio_input_kprobe(void)
{
    uint64_t base = drv_mmio_map(VINPUT_DUAL_PHYS, VINPUT_DUAL_SIZE, 3);
    if (!base)
        return;
    vmmio_probe_t p;
    if (vmmio_probe(base, &p) != 0 || p.device_id != VIRTIO_INPUT_DEVICE_ID)
        return; /* slot empty: nothing to report */
    char name[64];
    uint32_t n = vinput_cfg_string(base, VIRTIO_INPUT_CFG_ID_NAME,
                                   name, sizeof(name));
    printf("[UINPUT] kernel-placement probe: id=%u version=%u name=%s\n",
           p.device_id, p.version, n ? name : "?");
}
