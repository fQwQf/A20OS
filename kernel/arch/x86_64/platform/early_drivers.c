/*
 * x86_64 Early DriverStore overlay.  See
 * kernel/arch/riscv64/platform/early_drivers.c for the layout contract.
 */

#include "fs/rootfs_overlay.h"

#ifdef CONFIG_DRIVER_DEPLOYMENT_GENERIC

extern const unsigned char _binary_pc_spkr_a20drv_start[], _binary_pc_spkr_a20drv_end[];
extern const unsigned char _binary_virtio_blk_a20drv_start[], _binary_virtio_blk_a20drv_end[];
extern const unsigned char _binary_virtio_scsi_a20drv_start[], _binary_virtio_scsi_a20drv_end[];
extern const unsigned char _binary_ahci_a20drv_start[], _binary_ahci_a20drv_end[];

#define EARLY_DRIVER_ENTRY(path_, sym_) \
    { path_, _binary_##sym_##_a20drv_start, \
      (size_t)(uintptr_t)_binary_##sym_##_a20drv_end, 0644 }

rootfs_overlay_entry_t g_rootfs_driver_overlay[] = {
    EARLY_DRIVER_ENTRY("/boot/drivers/pc-spkr.a20drv", pc_spkr),
    EARLY_DRIVER_ENTRY("/boot/drivers/virtio-blk.a20drv", virtio_blk),
    EARLY_DRIVER_ENTRY("/boot/drivers/virtio-scsi.a20drv", virtio_scsi),
    EARLY_DRIVER_ENTRY("/boot/drivers/ahci.a20drv", ahci),
};

const size_t g_rootfs_driver_overlay_count =
    sizeof(g_rootfs_driver_overlay) / sizeof(g_rootfs_driver_overlay[0]);

void rootfs_driver_overlay_init(void)
{
    for (unsigned i = 0; i < g_rootfs_driver_overlay_count; i++) {
        rootfs_overlay_entry_t *e = &g_rootfs_driver_overlay[i];
        e->size = e->size - (uintptr_t)e->content;
    }
}

#else

rootfs_overlay_entry_t g_rootfs_driver_overlay[] = {};
const size_t g_rootfs_driver_overlay_count = 0;
void rootfs_driver_overlay_init(void) {}

#endif /* CONFIG_DRIVER_DEPLOYMENT_GENERIC */
