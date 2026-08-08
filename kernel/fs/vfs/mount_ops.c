/*
 * A20OS — VFS mount operations
 *
 * This file was mechanically extracted from vfs.c.
 */
#include "fs/vfs.h"
#include "fs/vfs/dcache.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/file.h"
#include "fs/file.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
#include "fs/locks.h"
#include "fs/fat32.h"
#include "fs/ext4.h"
#include "fs/ntfs.h"
#include "fs/isofs.h"
#include "fs/ramfs.h"
#include "fs/devfs.h"
#include "fs/procfs.h"
#include "fs/mount_setup.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"

extern vnode_t *cgroupfs_mount(int is_v2, const char *opts, void **out_sb);
extern vnode_t *procfs_mount(void);
extern vnode_t *sysfs_mount(void);

/* ============================================================
 * VFS Mount
 * ============================================================ */

static int parse_block_dev(const char *dev, int *out_idx, int *out_part) {
    if (strncmp(dev, "/dev/vd", 7) != 0) return -1;
    const char *p = dev + 7;
    if (*p < 'a' || *p > 'z') return -1;
    *out_idx = *p - 'a';
    p++;
    *out_part = 0;
    if (*p >= '1' && *p <= '9') {
        *out_part = *p - '0';
        p++;
    }
    if (*p != '\0') return -1;
    return 0;
}

static int mount_block_dev_idx(int dev_idx, const char *path, const char *fstype) {
    block_dev_t *bdev = mount_setup_block_device(dev_idx);
    if (!bdev) return -ENODEV;
    bcache_t *bc = bcache_create(bdev);
    if (!bc) return -ENOMEM;

    int r;
    if (fstype && fstype[0]) {
        r = vfs_mount_bc(path, fstype, bc);
    } else {
        r = vfs_mount_bc(path, "ext4", bc);
        if (r < 0) {
            bcache_destroy(bc);
            bc = bcache_create(bdev);
            if (!bc) return -ENOMEM;
            r = vfs_mount_bc(path, "vfat", bc);
        }
    }
    if (r < 0) bcache_destroy(bc);
    return r;
}

int vfs_mount(const char *dev, const char *path, const char *fstype, int flags, const char *data) {
    if (!path || !fstype) return -EINVAL;
    if (strcmp(fstype, "cgroup") == 0 || strcmp(fstype, "cgroup2") == 0) {
        vnode_t *target = vfs_resolve(path);
        if (!target) return -ENOENT;
        int is_dir = target->type == VFS_FT_DIR;
        vnode_put(target);
        if (!is_dir) return -ENOTDIR;

        extern vnode_t *cgroupfs_mount(int is_v2, const char *opts, void **out_sb);
        extern void cgroupfs_unmount(vnode_t *root);
        int is_v2 = (strcmp(fstype, "cgroup2") == 0);
        const char *opts = (data && *data) ? data : (dev ? dev : "");
        void *sb_ptr = NULL;
        vnode_t *root = cgroupfs_mount(is_v2, opts, &sb_ptr);
        if (!root) return -ENOMEM;

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) { cgroupfs_unmount(root); return -ENOMEM; }
        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type = FS_TYPE_CGROUP;
        mnt->flags = flags;
        mnt->root = root;
        mnt->root->mnt = mnt;
        mnt->fs_data = sb_ptr;
        strncpy(mnt->dev, dev ? dev : "none", sizeof(mnt->dev) - 1);
        mnt->dev[sizeof(mnt->dev) - 1] = '\0';
        strncpy(mnt->fstype, fstype, sizeof(mnt->fstype) - 1);
        mnt->fstype[sizeof(mnt->fstype) - 1] = '\0';
        if (is_v2) {
            strncpy(mnt->opts, "rw,memory_recursiveprot", sizeof(mnt->opts) - 1);
        } else {
            const char *cg_opts = (data && *data) ? data : "memory,cpuset,cpu,cpuacct";
            strncpy(mnt->opts, cg_opts, sizeof(mnt->opts) - 1);
        }
        mnt->opts[sizeof(mnt->opts) - 1] = '\0';
        vfs_dcache_invalidate_all();
        return 0;
    }
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *existing = vfs_mount_at(i);
        if (existing && strcmp(existing->path, path) == 0) {
            existing->flags = flags;
            vfs_dcache_invalidate_all();
            return 0;
        }
    }
    /*
     * A final-round image is used as a chroot rather than replacing A20OS's
     * bootstrap ramfs.  Allow procfs, sysfs and devfs to be mounted below
     * that root so absolute paths keep normal Linux rootfs semantics after
     * chroot(2).
     */
    if (strcmp(fstype, "proc") == 0 ||
        strcmp(fstype, "sysfs") == 0 ||
        strcmp(fstype, "devtmpfs") == 0 ||
        strcmp(fstype, "devfs") == 0) {
        vnode_t *target = vfs_resolve(path);
        if (!target) return -ENOENT;
        int is_dir = target->type == VFS_FT_DIR;
        vnode_put(target);
        if (!is_dir) return -ENOTDIR;

        vnode_t *root = NULL;
        int type = 0;
        if (strcmp(fstype, "proc") == 0) {
            root = procfs_mount();
            type = FS_TYPE_PROCFS;
        } else if (strcmp(fstype, "sysfs") == 0) {
            root = sysfs_mount();
            type = FS_TYPE_SYSFS;
        } else {
            root = devfs_mount();
            type = FS_TYPE_DEVFS;
        }
        if (!root) return -ENOMEM;

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type = type;
        mnt->flags = flags;
        mnt->root = root;
        root->mnt = mnt;
        strncpy(mnt->dev, dev ? dev : "none", sizeof(mnt->dev) - 1);
        mnt->dev[sizeof(mnt->dev) - 1] = '\0';
        strncpy(mnt->fstype, fstype, sizeof(mnt->fstype) - 1);
        mnt->fstype[sizeof(mnt->fstype) - 1] = '\0';
        strncpy(mnt->opts, "rw", sizeof(mnt->opts) - 1);
        mnt->opts[sizeof(mnt->opts) - 1] = '\0';
        vfs_dcache_invalidate_all();
        return 0;
    }
    if (strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "ramfs") == 0) {
        vnode_t *target = vfs_resolve(path);
        if (!target) return -ENOENT;
        int is_dir = target->type == VFS_FT_DIR;
        vnode_put(target);
        if (!is_dir) return -ENOTDIR;

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type = FS_TYPE_RAMFS;
        strncpy(mnt->dev, dev ? dev : "none", sizeof(mnt->dev) - 1);
        mnt->dev[sizeof(mnt->dev) - 1] = '\0';
        strncpy(mnt->fstype, fstype, sizeof(mnt->fstype) - 1);
        mnt->fstype[sizeof(mnt->fstype) - 1] = '\0';
        strncpy(mnt->opts, "rw", sizeof(mnt->opts) - 1);
        mnt->flags = flags;
        mnt->root = ramfs_mount_empty(mnt);
        if (!mnt->root) {
            vfs_mount_remove(mnt);
            return -ENOMEM;
        }
        mnt->root->mnt = mnt;
        vfs_dcache_invalidate_all();
        return 0;
    }
    /* Try block device mount: /dev/vdX[N] */
    {
        int dev_idx = 0, part_num = 0;
        if (parse_block_dev(dev, &dev_idx, &part_num) == 0) {
            int r = mount_block_dev_idx(dev_idx, path, fstype);
            if (r < 0 && part_num > 1) {
                int compat_idx = dev_idx + part_num - 1;
                if (compat_idx != dev_idx)
                    r = mount_block_dev_idx(compat_idx, path, fstype);
            }
            return r;
        }
    }
    return -EINVAL;
}

int vfs_mount_bc(const char *path, const char *fstype, bcache_t *bc) {
    if (strcmp(fstype, "fat32") == 0 || strcmp(fstype, "vfat") == 0) {
        if (!bc) { kdebug("[VFS] No bcache for FAT32 mount\n"); return -ENODEV; }

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = fat32_mount(bc);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_FAT32;
        strncpy(mnt->dev, "/dev/vda", sizeof(mnt->dev) - 1);
        strncpy(mnt->fstype, "vfat", sizeof(mnt->fstype) - 1);
        strncpy(mnt->opts, "rw,relatime,fmask=0022,dmask=0022,codepage=437,iocharset=ascii,shortname=mixed,errors=remount-ro", sizeof(mnt->opts) - 1);
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted FAT32 at %s\n", path);
        vfs_dcache_invalidate_all();
        return 0;
    }

    if (strcmp(fstype, "ntfs") == 0) {
        if (!bc) { kdebug("[VFS] No bcache for NTFS mount\n"); return -ENODEV; }

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = ntfs_mount(bc);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_NTFS;
        strncpy(mnt->dev, "/dev/vda", sizeof(mnt->dev) - 1);
        strncpy(mnt->fstype, "ntfs", sizeof(mnt->fstype) - 1);
        strncpy(mnt->opts, "rw", sizeof(mnt->opts) - 1);
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted NTFS at %s\n", path);
        vfs_dcache_invalidate_all();
        return 0;
    }

    if (strcmp(fstype, "ext4") == 0) {
        if (!bc) { kdebug("[VFS] No bcache for ext4 mount\n"); return -ENODEV; }

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = ext4_mount(bc);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_EXT4;
        strncpy(mnt->dev, "/dev/vda", sizeof(mnt->dev) - 1);
        strncpy(mnt->fstype, "ext4", sizeof(mnt->fstype) - 1);
        strncpy(mnt->opts, "rw,relatime", sizeof(mnt->opts) - 1);
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted ext4 at %s\n", path);
        vfs_dcache_invalidate_all();
        return 0;
    }

    if (strcmp(fstype, "isofs") == 0 || strcmp(fstype, "iso9660") == 0) {
        if (!bc) { kdebug("[VFS] No bcache for isofs mount\n"); return -ENODEV; }

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = isofs_mount(bc);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_ISOFS;
        strncpy(mnt->dev, "/dev/cdrom", sizeof(mnt->dev) - 1);
        strncpy(mnt->fstype, "iso9660", sizeof(mnt->fstype) - 1);
        strncpy(mnt->opts, "ro", sizeof(mnt->opts) - 1);
        mnt->flags = 1;               /* read-only */
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted iso9660 at %s\n", path);
        vfs_dcache_invalidate_all();
        return 0;
    }

    kdebug("[VFS] Unknown fstype: %s\n", fstype);
    return -EINVAL;
}

int vfs_umount(const char *path) {
    if (!path) return -EINVAL;
    char norm_path[MAX_PATH_LEN];
    strncpy(norm_path, path, MAX_PATH_LEN - 1);
    norm_path[MAX_PATH_LEN - 1] = '\0';

    size_t len = strlen(norm_path);
    while (len > 1 && norm_path[len - 1] == '/') {
        norm_path[len - 1] = '\0';
        len--;
    }

    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *mnt = vfs_mount_at(i);
        if (!mnt) continue;

        char mnt_norm[MAX_PATH_LEN];
        strncpy(mnt_norm, mnt->path, MAX_PATH_LEN - 1);
        mnt_norm[MAX_PATH_LEN - 1] = '\0';
        size_t mnt_len = strlen(mnt_norm);
        while (mnt_len > 1 && mnt_norm[mnt_len - 1] == '/') {
            mnt_norm[mnt_len - 1] = '\0';
            mnt_len--;
        }

        if (strcmp(mnt_norm, norm_path) == 0) {
            vfs_dcache_invalidate_all();
            vnode_t *root = mnt->root;
            if (mnt->type == FS_TYPE_FAT32) {
                fat32_unmount(root);
            } else if (mnt->type == FS_TYPE_EXT4) {
                ext4_unmount(root);
            } else if (mnt->type == FS_TYPE_NTFS) {
                ntfs_unmount(root);
            } else if (mnt->type == FS_TYPE_ISOFS) {
                isofs_unmount(root);
            }
            vfs_mount_remove(mnt);
            return 0;
        }
    }
    return -EINVAL;
}

/* Truncate */
int vfs_truncate(const char *path, size_t size) {
    vnode_t *vn = vfs_resolve(path);
    if (!vn) return -ENOENT;
    int r = -ENOSYS;
    if (vn->ops && vn->ops->truncate) r = vn->ops->truncate(vn, size);
    if (r == 0)
        page_cache_truncate(vn, size);
    vnode_put(vn);
    return r;
}

int vfs_ftruncate(int fd, size_t size) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf) return -EBADF;
    int r = 0;
    if (!vfs_should_write(vf->flags)) r = -EINVAL;
    else if (!vf->vnode || !vf->vnode->ops || !vf->vnode->ops->truncate) r = -EINVAL;
    else if ((vf->seals & F_SEAL_SHRINK) && size < vf->vnode->size) r = -EPERM;
    else if ((vf->seals & F_SEAL_GROW) && size > vf->vnode->size) r = -EPERM;
    else if (vf->seals & F_SEAL_WRITE) r = -EPERM;
    else r = vf->vnode->ops->truncate(vf->vnode, size);
    if (r == 0) {
        page_cache_truncate(vf->vnode, size);
    }
    vfs_put_file_ref(fd, vf);
    return r;
}
