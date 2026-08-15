/*
 * LoongArch32 Early DriverStore overlay.  No user-space driver blobs are
 * built for the NaiLoong target yet.
 */

#include "fs/rootfs_overlay.h"

rootfs_overlay_entry_t g_rootfs_driver_overlay[] = {};
const size_t g_rootfs_driver_overlay_count = 0;

void rootfs_driver_overlay_init(void)
{
}
