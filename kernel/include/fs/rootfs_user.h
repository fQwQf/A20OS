#ifndef FS_ROOTFS_USER_H
#define FS_ROOTFS_USER_H

#include "fs/rootfs_overlay.h"

extern rootfs_overlay_entry_t g_rootfs_user_overlay[];
extern const size_t g_rootfs_user_overlay_count;
void rootfs_user_overlay_init(void);

#endif
