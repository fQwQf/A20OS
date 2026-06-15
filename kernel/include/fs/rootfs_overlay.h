#ifndef FS_ROOTFS_OVERLAY_H
#define FS_ROOTFS_OVERLAY_H

#include "core/types.h"

typedef struct {
    const char *path;
    const unsigned char *content;
    size_t size;
    uint32_t mode;
} rootfs_overlay_entry_t;

extern const rootfs_overlay_entry_t g_rootfs_overlay[];
extern const size_t g_rootfs_overlay_count;

#endif
