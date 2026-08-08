/*
 * Early DriverStore overlay for this architecture.
 *
 * This arch has no .a20drv packages yet, so the overlay is empty.  The
 * symbols still exist so common code (ramfs_populate_overlay) links on every
 * profile.  See kernel/arch/riscv64/platform/early_drivers.c for the full
 * contract.
 */

#include "fs/rootfs_overlay.h"

rootfs_overlay_entry_t g_rootfs_driver_overlay[] = {};
const size_t g_rootfs_driver_overlay_count = 0;
void rootfs_driver_overlay_init(void) {}
