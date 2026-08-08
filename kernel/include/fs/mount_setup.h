#ifndef _MOUNT_SETUP_H
#define _MOUNT_SETUP_H

#include "drivers/block/block_dev.h"

/*
 * Block-device mount strategy, split out of kernel/main.c.
 *
 * Probes all virtio-blk / virtio-scsi / AHCI / USB-storage devices and
 * auto-detects filesystems:
 *   fat32 -> /bin   (our utilities: init, mksh, cmds, ...)
 *   ext4  -> /test  (judge sdcard or local sdcard image)
 *
 * The logic is independent of device ordering and works regardless of how the
 * disks are enumerated.  In BRINGUP mode there are no block devices and this
 * module compiles to nothing.
 */

#ifndef BRINGUP
void mount_block_devices(void);
int try_mount(block_dev_t *dev, const char *mnt, const char *fstype);
#endif

/* Class-based block-device lookup used by vfs_mount(/dev/vd*) and the swap
 * path.  Compiled in every profile; BRINGUP builds return NULL (no devices). */
block_dev_t *mount_setup_block_device(int index);

#endif /* _MOUNT_SETUP_H */
