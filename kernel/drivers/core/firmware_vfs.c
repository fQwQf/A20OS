/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/firmware.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/string.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/vfs/mntns.h"
#include "fs/vfs/mount.h"

static int firmware_vfs_open(void *context, const char *name,
                              void **stream, uint64_t *size)
{
    vfile_t *directory = context;
    /* Keep shutdown from closing backends throughout lookup/open/read/close.
     * This is an admission count, not a lock held over filesystem I/O. */
    int ret = mntns_fileop_enter();
    if (ret)
        return ret;
    vnode_t *node = NULL;
    vfile_t *file = NULL;
    if (directory->backend_close_done) {
        ret = -ESHUTDOWN;
        goto fail;
    }
    node = directory->vnode;
    vnode_get(node);
    /* load() validated the entire relative name before invoking the provider. */
    char path[DRIVER_FIRMWARE_NAME_MAX + 1];
    memcpy(path, name, strlen(name) + 1);
    char *part = path;
    while (*part) {
        char *end = strchr(part, '/');
        if (end)
            *end = '\0';
        if (node->type != VFS_FT_DIR) {
            ret = -ENOTDIR;
            goto fail;
        }
        if (!node->ops || !node->ops->lookup) {
            ret = -EOPNOTSUPP;
            goto fail;
        }
        vnode_t *child = NULL;
        ret = node->ops->lookup(node, part, &child);
        if (ret)
            goto fail;
        vnode_put(node);
        node = child;
        if (!node) {
            ret = -EIO;
            goto fail;
        }
        if (node->mnt != directory->vnode->mnt) {
            ret = -EXDEV;
            goto fail;
        }
        if (node->type == VFS_FT_SYMLINK) {
            ret = -ELOOP;
            goto fail;
        }
        if (!end)
            break;
        part = end + 1;
    }
    /* Some filesystems use VFS_FT_REGULAR for FIFOs and device nodes too.
     * Refuse those before open can block or cause device side effects. */
    if (node->type != VFS_FT_REGULAR || (node->mode & S_IFMT) != S_IFREG) {
        ret = -EINVAL;
        goto fail;
    }
    if (!node->ops || !node->ops->open) {
        ret = -EOPNOTSUPP;
        goto fail;
    }
    /* A private read-only vfile: no descriptor is installed in the caller. */
    file = node->ops->open(node, O_RDONLY);
    if (!file) {
        ret = -ENOMEM;
        goto fail;
    }
    if (file->vnode != node) {
        ret = -EIO;
        goto fail;
    }
    if (node->mnt) {
        vfs_mount_vfile_get(node->mnt);
        file->mount_open_counted = 1;
    }
    kstat_t stat;
    ret = vfs_vfile_stat(file, &stat);
    if (ret)
        goto fail;
    *size = stat.st_size;
    *stream = file;
    vnode_put(node);
    return 0;
fail:
    if (file)
        vfs_put_file(file);
    if (node)
        vnode_put(node);
    mntns_fileop_leave();
    return ret;
}

static int firmware_vfs_read(void *context, void *stream, uint64_t offset,
                              void *buffer, size_t count)
{
    (void)context;
    return vfs_pread_file(stream, buffer, count, offset);
}

static void firmware_vfs_close(void *context, void *stream)
{
    (void)context;
    vfs_put_file(stream);
    mntns_fileop_leave();
}

static void firmware_vfs_release(void *context)
{
    vfs_put_file(context);
}

int driver_firmware_source_create_vfs(vfile_t *directory,
                                      struct driver_firmware_source **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!directory || !directory->vnode ||
        directory->vnode->type != VFS_FT_DIR)
        return -ENOTDIR;
    /* A raw vnode is insufficient: require the normal open-file mount pin. */
    if (!directory->vnode->mnt || !directory->mount_open_counted)
        return -EINVAL;
    int ret = mntns_fileop_enter();
    if (ret)
        return ret;
    if (directory->backend_close_done) {
        mntns_fileop_leave();
        return -ESHUTDOWN;
    }
    const struct driver_firmware_ops ops = {
        .open = firmware_vfs_open, .read = firmware_vfs_read,
        .close = firmware_vfs_close, .release = firmware_vfs_release,
    };
    vfile_get(directory);
    ret = driver_firmware_source_create(&ops, directory, out);
    if (ret)
        vfs_put_file(directory);
    mntns_fileop_leave();
    return ret;
}
#endif
