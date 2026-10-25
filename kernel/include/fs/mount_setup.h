#ifndef _MOUNT_SETUP_H
#define _MOUNT_SETUP_H

#include "drivers/block/block_dev.h"

/*
 * Block-device mount strategy, split out of kernel/main.c.
 *
 * Probes all virtio-blk / virtio-scsi / AHCI / USB-storage devices and
 * auto-detects filesystems:
 *   fat32 -> /bin   (our utilities: init, mksh, cmds, ...)
 *   ext4  -> /extra  (local sdcard image)
 *
 * The logic is independent of device ordering and works regardless of how the
 * disks are enumerated.  BRINGUP mode skips automatic root mounts; explicitly
 * requested mounts may still use probed class devices.
 */

#ifndef BRINGUP
void mount_block_devices(void);
int try_mount(block_dev_t *dev, const char *mnt, const char *fstype);
#endif

/* Class-based block-device lookup used by vfs_mount(/dev/vd*) and the swap
 * path.  Compiled in every profile; returns NULL when no matching device is probed. */
block_dev_t *mount_setup_block_device(int index);

/* Probe DOS/MBR storage and mount its first Linux partition read-only at
 * /extra.  Intended for explicitly selected RAM-only board experiments. */
void mount_read_only_storage(void);
/* Optional RAMFS media input: first raw/GPT FAT32 volume, read-only /media. */
void mount_ramfs_media(void);

#endif /* _MOUNT_SETUP_H */
