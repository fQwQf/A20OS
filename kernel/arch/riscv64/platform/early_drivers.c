/*
 * RISC-V 64 Early DriverStore overlay.
 *
 * The early .a20drv packages are linked into the kernel binary (objcopy
 * --rename-section into .rodata.rootfs_drivers) and exposed as files under
 * /boot/drivers in the root ramfs, so the driver manager can load root-device
 * drivers before the real root disk is mounted.  The table is arch-specific
 * because each platform has its own early driver set; common code only
 * consumes g_rootfs_driver_overlay (kernel/include/fs/rootfs_overlay.h).
 *
 * Symbol names derive from the package file names: '-' and '.' become '_'
 * (virtio-blk.a20drv -> _binary_virtio_blk_a20drv_start).  The size field
 * temporarily holds the end address (an address constant); the init function
 * converts it to end - start, since the difference of two symbols is not a
 * valid constant initializer.
 */

#include "fs/rootfs_overlay.h"

#ifdef CONFIG_DRIVER_DEPLOYMENT_GENERIC

extern const unsigned char _binary_rtc_a20drv_start[], _binary_rtc_a20drv_end[];
extern const unsigned char _binary_virtio_blk_a20drv_start[], _binary_virtio_blk_a20drv_end[];
extern const unsigned char _binary_virtio_scsi_a20drv_start[], _binary_virtio_scsi_a20drv_end[];
extern const unsigned char _binary_dw_sdio_a20drv_start[], _binary_dw_sdio_a20drv_end[];

#define EARLY_DRIVER_ENTRY(path_, sym_) \
    { path_, _binary_##sym_##_a20drv_start, \
      (size_t)(uintptr_t)_binary_##sym_##_a20drv_end, 0644 }

rootfs_overlay_entry_t g_rootfs_driver_overlay[] = {
    EARLY_DRIVER_ENTRY("/boot/drivers/rtc.a20drv", rtc),
    EARLY_DRIVER_ENTRY("/boot/drivers/virtio-blk.a20drv", virtio_blk),
    EARLY_DRIVER_ENTRY("/boot/drivers/virtio-scsi.a20drv", virtio_scsi),
    EARLY_DRIVER_ENTRY("/boot/drivers/dw-sdio.a20drv", dw_sdio),
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
