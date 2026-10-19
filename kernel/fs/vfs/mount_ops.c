/*
 * A20OS — VFS mount operations
 *
 * This file was mechanically extracted from vfs.c.
 */
#include "fs/quota.h"
#include "fs/vfs.h"
#include "fs/vfs/dcache.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/mntns.h"
#include "fs/vfs/mount_state.h"
#include "fs/vfs/file.h"
#include "fs/vfs/stat_perm.h"
#include "fs/file.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
#include "fs/locks.h"
#include "fs/fat32.h"
#include "fs/ext4.h"
#include "fs/lfs_vfs.h"
#include "fs/ntfs.h"
#include "fs/isofs.h"
#include "fs/uxfs.h"
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

static int mount_block_dev_idx(int dev_idx, const char *path,
                               const char *fstype, int flags) {
    block_dev_t *bdev = mount_setup_block_device(dev_idx);
    if (!bdev) return -ENODEV;
    bcache_t *bc = bcache_create(bdev);
    if (!bc) return -ENOMEM;

    int r = vfs_mount_bc_flags(path, fstype, bc, flags);
    if (r < 0) bcache_destroy(bc);
    return r;
}

static int mount_target_is_dir(const char *path)
{
    vnode_t *target = vfs_resolve(path);
    if (!target) return -ENOENT;
    int is_dir = target->type == VFS_FT_DIR;
    vnode_put(target);
    return is_dir ? 0 : -ENOTDIR;
}

static int mount_cgroup_backend(const char *dev, const char *path,
                                const char *fstype, int flags, const char *data)
{
    int r = mount_target_is_dir(path);
    if (r < 0) return r;
    extern vnode_t *cgroupfs_mount(int is_v2, const char *opts, void **out_sb);
    extern void cgroupfs_unmount(vnode_t *root);
    int is_v2 = strcmp(fstype, "cgroup2") == 0;
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
    if (is_v2)
        strncpy(mnt->opts, "rw,memory_recursiveprot", sizeof(mnt->opts) - 1);
    else
        strncpy(mnt->opts, (data && *data) ? data : "memory,cpuset,cpu,cpuacct", sizeof(mnt->opts) - 1);
    mnt->opts[sizeof(mnt->opts) - 1] = '\0';
    vfs_mount_link_tree(mnt);
    vfs_dcache_invalidate_all();
    return 0;
}

static int mount_virtual_backend(const char *dev, const char *path,
                                 const char *fstype, int flags, const char *data)
{
    (void)data;
    int r = mount_target_is_dir(path);
    if (r < 0) return r;
    /* devpts is a compatibility mount; slave devices are supplied by devfs. */
    if (strcmp(fstype, "devpts") == 0) return 0;
    vnode_t *root = NULL;
    int type = 0;
    if (strcmp(fstype, "proc") == 0) {
        root = procfs_mount(); type = FS_TYPE_PROCFS;
    } else if (strcmp(fstype, "sysfs") == 0) {
        root = sysfs_mount(); type = FS_TYPE_SYSFS;
    } else {
        root = devfs_mount(); type = FS_TYPE_DEVFS;
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
    vfs_mount_link_tree(mnt);
    vfs_dcache_invalidate_all();
    return 0;
}

static int mount_ram_backend(const char *dev, const char *path,
                             const char *fstype, int flags, const char *data)
{
    (void)data;
    int r = mount_target_is_dir(path);
    if (r < 0) return r;
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
    vfs_mount_link_tree(mnt);
    vfs_dcache_invalidate_all();
    return 0;
}

static int mount_block_backend(int dev_idx, int part_num, const char *path,
                               const char *fstype, int flags)
{
    (void)part_num;
    return mount_block_dev_idx(dev_idx, path, fstype, flags);
}

static int mount_update_existing(const char *path, int flags)
{
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *existing = vfs_mount_at(i);
        if (existing && strcmp(existing->path, path) == 0) {
            existing->flags = flags;
            vfs_dcache_invalidate_all();
            return 1;
        }
    }
    return 0;
}

static const struct vfs_mount_dispatch_ops g_vfs_mount_dispatch_ops = {
    .update_existing = mount_update_existing,
    .mount_cgroup = mount_cgroup_backend,
    .mount_virtual = mount_virtual_backend,
    .mount_ram = mount_ram_backend,
    .mount_block = mount_block_backend,
};
_Static_assert(sizeof(g_vfs_mount_dispatch_ops) / sizeof(void *) == 5,
               "vfs mount dispatch mirror: ops table field count");

int vfs_mount(const char *dev, const char *path, const char *fstype, int flags, const char *data)
{
    return a20_vfs_mount_dispatch(&g_vfs_mount_dispatch_ops, dev, path, fstype, flags, data);
}

int vfs_mount_bc_flags(const char *path, const char *fstype, bcache_t *bc,
                       int flags) {
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
        strncpy(mnt->opts, (flags & VFS_MOUNT_RDONLY) ? "ro" :
                "rw,relatime,fmask=0022,dmask=0022,codepage=437,iocharset=ascii,shortname=mixed,errors=remount-ro", sizeof(mnt->opts) - 1);
        mnt->flags = flags;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted FAT32 at %s\n", path);
        vfs_mount_link_tree(mnt);
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
        strncpy(mnt->opts, (flags & VFS_MOUNT_RDONLY) ? "ro" : "rw",
                sizeof(mnt->opts) - 1);
        mnt->flags = flags;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted NTFS at %s\n", path);
        vfs_mount_link_tree(mnt);
        vfs_dcache_invalidate_all();
        return 0;
    }

    if (strcmp(fstype, "ext4") == 0) {
        if (!bc) { kdebug("[VFS] No bcache for ext4 mount\n"); return -ENODEV; }
        if (uxfs_block_device_owned(bc->dev))
            return -EBUSY;

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = ext4_mount_flags(bc, flags);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_EXT4;
        strncpy(mnt->dev, "/dev/vda", sizeof(mnt->dev) - 1);
        strncpy(mnt->fstype, "ext4", sizeof(mnt->fstype) - 1);
        strncpy(mnt->opts, (flags & VFS_MOUNT_RDONLY) ? "ro,relatime" :
                "rw,relatime", sizeof(mnt->opts) - 1);
        mnt->flags = flags;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted ext4 at %s\n", path);
        vfs_mount_link_tree(mnt);
        vfs_dcache_invalidate_all();
        return 0;
    }

    if (strcmp(fstype, "littlefs") == 0) {
        if (!bc) { kdebug("[VFS] No bcache for littlefs mount\n"); return -ENODEV; }

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = littlefs_mount(bc);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_LITTLEFS;
        strncpy(mnt->dev, "/dev/vda", sizeof(mnt->dev) - 1);
        strncpy(mnt->fstype, "littlefs", sizeof(mnt->fstype) - 1);
        strncpy(mnt->opts, (flags & VFS_MOUNT_RDONLY) ? "ro,relatime" :
                "rw,relatime", sizeof(mnt->opts) - 1);
        mnt->flags = flags;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;
        vnode_get(root);  /* mount holds a persistent reference */

        kdebug("[VFS] Mounted littlefs at %s\n", path);
        vfs_mount_link_tree(mnt);
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
        vfs_mount_link_tree(mnt);
        vfs_dcache_invalidate_all();
        return 0;
    }

    kdebug("[VFS] Unknown fstype: %s\n", fstype);
    return -EINVAL;
}

int vfs_mount_bc(const char *path, const char *fstype, bcache_t *bc) {
    return vfs_mount_bc_flags(path, fstype, bc, 0);
}

/* Release one namespace's hold on a mount entry. A copied entry carries an
 * explicit shared_source pointer, so the source mount (and vnode->mnt target)
 * stays alive until every copied table detaches. */
static void *mt_shared_source(void *p) { return ((mount_t *)p)->shared_source; }
static int mt_ns_users(void *p) { return __atomic_load_n(&((mount_t *)p)->ns_users, __ATOMIC_ACQUIRE); }
static int mt_teardown_running(void *p) { return ((mount_t *)p)->teardown_running; }
static void mt_set_teardown_running(void *p, int value) { ((mount_t *)p)->teardown_running = value; }
static int mt_mount_kind(void *p) { return ((mount_t *)p)->type; }
static void *mt_mount_root(void *p) { return ((mount_t *)p)->root; }
static void mt_set_ns_users_zero(void *p) { __atomic_store_n(&((mount_t *)p)->ns_users, 0, __ATOMIC_RELEASE); }

static int mt_unmount_backend(int kind, void *root)
{
    int ret = 0;
    if (kind == FS_TYPE_FAT32) {
        fat32_unmount((vnode_t *)root);
    } else if (kind == FS_TYPE_EXT4) {
        ret = ext4_unmount((vnode_t *)root);
    } else if (kind == FS_TYPE_NTFS) {
        ntfs_unmount((vnode_t *)root);
    } else if (kind == FS_TYPE_ISOFS) {
        isofs_unmount((vnode_t *)root);
    } else if (kind == FS_TYPE_UXFS) {
        extern int uxfs_unmount(struct vnode *root);
        ret = uxfs_unmount((vnode_t *)root);
    } else if (kind == FS_TYPE_LITTLEFS) {
        littlefs_unmount((vnode_t *)root);
    }
    return ret;
}

static void mt_teardown_trace(void *p, int phase, int users)
{
#ifdef CONFIG_MNTNS_TEST
    mount_t *mnt = (mount_t *)p;
    if (strcmp(mnt->path, "/tmp/ns_shared") == 0) {
        if (phase == 0)
            printf("MNTNS_TEST: FS_TEARDOWN path=%s users=%d\n", mnt->path, users);
        else
            printf("MNTNS_TEST: SOURCE_FS_UNMOUNTED users=0\n");
    }
#else
    (void)p; (void)phase; (void)users;
#endif
}

static const struct vfs_mount_fs_teardown_ops g_mount_fs_teardown_ops = {
    .lock = mntns_mount_table_lock,
    .unlock = mntns_mount_table_unlock,
    .shared_source = mt_shared_source,
    .users = mt_ns_users,
    .is_running = mt_teardown_running,
    .set_running = mt_set_teardown_running,
    .kind = mt_mount_kind,
    .root = mt_mount_root,
    .unmount_backend = mt_unmount_backend,
    .set_users_zero = mt_set_ns_users_zero,
    .trace = mt_teardown_trace,
};
_Static_assert(sizeof(g_mount_fs_teardown_ops) / sizeof(void *) == 11,
               "mount fs teardown mirror: ops table field count");

int vfs_mount_fs_teardown(mount_t *mnt)
{
    return a20_vfs_mount_fs_teardown(&g_mount_fs_teardown_ops, mnt);
}

/* Drop every hold this namespace's table has, so a namespace going away
 * cannot leak a shared filesystem forever.  Copied entries detach without
 * running the destructor; the source entry's ns_users is decremented when its
 * own namespace unmounts or is torn down. */
static void ns_source_users_restore(void *p)
{
    mount_t *source = (mount_t *)p;
    __atomic_add_fetch(&source->ns_users, 1, __ATOMIC_RELAXED);
}
static void ns_clear_shared_source(void *p) { ((mount_t *)p)->shared_source = NULL; }

static void *ns_shared_source_get(void *p) { return ((mount_t *)p)->shared_source; }

#ifdef CONFIG_MNTNS_TEST
static void ns_log_source_drop(int old, mount_t *source)
{
    if (strcmp(source->path, "/tmp/ns_shared") == 0)
        printf("MNTNS_TEST: DROP_SOURCE_PIN old=%d now=%d\n", old, old - 1);
}
#endif

static int ns_source_users_dec_logged(void *p)
{
    mount_t *source = (mount_t *)p;
    int old = __atomic_fetch_sub(&source->ns_users, 1, __ATOMIC_ACQ_REL);
#ifdef CONFIG_MNTNS_TEST
    ns_log_source_drop(old, source);
#endif
    return old;
}

static int ns_mount_count(void *p) { return ((mnt_namespace_t *)p)->nmounts; }
static void *ns_mount_at(void *p, int i)
{
    mnt_namespace_t *ns = (mnt_namespace_t *)p;
    return i >= 0 && i < ns->nmounts ? ns->mounts[i] : NULL;
}
static void *ns_dead_head(void *p) { return ((mnt_namespace_t *)p)->dead_mounts; }
static void *ns_dead_next(void *p) { return ((mount_t *)p)->dead_next; }
static int ns_fs_teardown(void *p) { return vfs_mount_fs_teardown((mount_t *)p); }

static const struct vfs_mount_namespace_teardown_ops g_ns_teardown_ops = {
    .mount_count = ns_mount_count,
    .mount_at = ns_mount_at,
    .dead_head = ns_dead_head,
    .dead_next = ns_dead_next,
    .shared_source = ns_shared_source_get,
    .fs_teardown = ns_fs_teardown,
    .decrement_source_users = ns_source_users_dec_logged,
    .restore_source_users = ns_source_users_restore,
    .clear_shared_source = ns_clear_shared_source,
};
_Static_assert(sizeof(g_ns_teardown_ops) / sizeof(void *) == 9,
               "mount namespace teardown mirror: ops table field count");

int vfs_mount_namespace_teardown(mnt_namespace_t *ns)
{
    return a20_vfs_mount_namespace_teardown(&g_ns_teardown_ops, ns);
}

/*
 * Why a mount cannot be unmounted right now.  A20OS tracks process roots as
 * mount references (mount_t::root_users) and mounts as real tree nodes, so
 * the three Linux busy reasons become direct questions instead of guesses:
 *
 *   - somebody's root:  root_users > 1 (the extra reference is the process)
 *   - a submount below: mnt_child != NULL
 *   - the path is not a mount point at all
 *
 * @flags carries the Linux MNT_* bits; MNT_DETACH waives all three, which is
 * what pivot_root(2) and lazy-unmount callers want.
 */

/* Snapshot the visible mounts and let the engine decide which one matches and
 * whether the umount is admitted.  C keeps the table walk and the per-mount
 * counters; the matching and the busy-rule order are Rust-owned. */
static int vfs_umount_plan_build(const char *path, int flags, int check_busy,
                                 mount_t **out_mnt, vfs_umount_plan_t *out_plan)
{
    vfs_mount_cand_t cands[MNTNS_MAX_MOUNTS];
    mount_t *sel[MNTNS_MAX_MOUNTS];
    int count = vfs_mount_count();
    int n = 0;
    for (int i = 0; i < count && n < MNTNS_MAX_MOUNTS; i++) {
        mount_t *mnt = vfs_mount_at(i);
        if (!mnt)
            continue;
        sel[n] = mnt;
        cands[n].path = mnt->path;
        cands[n].root_users = (uint32_t)vfs_mount_root_users(mnt);
        cands[n].has_child = vfs_mount_first_child(mnt) ? 1u : 0u;
        cands[n].cwd_users = (uint32_t)vfs_mount_cwd_users(mnt);
        cands[n].is_ns_root =
            (!mnt->mnt_parent && strcmp(mnt->path, "/") == 0) ? 1u : 0u;
        n++;
    }
    int r = a20_vfs_umount_plan(path, flags, check_busy, cands, (size_t)n, out_plan);
    if (r < 0)
        return r;
    *out_mnt = sel[out_plan->index];
    return 0;
}

static int umount_teardown(void *mnt) { return vfs_mount_fs_teardown((mount_t *)mnt); }
static void umount_invalidate(void) { vfs_dcache_invalidate_all(); }
static void umount_drop_time_meta(void *mnt) { vfs_drop_time_meta_mount((mount_t *)mnt); }
static void umount_remove(void *mnt) { vfs_mount_remove((mount_t *)mnt); }
static void umount_retry_pending(void) { mntns_retry_pending(); }

static const struct vfs_umount_execute_ops g_vfs_umount_execute_ops = {
    .teardown = umount_teardown,
    .invalidate_all = umount_invalidate,
    .drop_time_meta = umount_drop_time_meta,
    .remove = umount_remove,
    .retry_pending = umount_retry_pending,
};
_Static_assert(sizeof(g_vfs_umount_execute_ops) / sizeof(void *) == 5,
               "vfs umount executor mirror: ops table field count");

static int vfs_umount_execute_plan(mount_t *mnt, vfs_umount_plan_t *plan)
{
    return vfs_mount_execute_umount(&g_vfs_umount_execute_ops, mnt, plan);
}

int vfs_umount_flags(const char *path, int flags) {
    if (!path) return -EINVAL;
    mount_t *mnt = NULL;
    vfs_umount_plan_t plan;
    int r = vfs_umount_plan_build(path, flags, 1, &mnt, &plan);
    if (r < 0)
        return r;
    return vfs_umount_execute_plan(mnt, &plan);
}

int vfs_umount(const char *path) {
    if (!path) return -EINVAL;
    /* The legacy entry point skips the busy rules, exactly as before. */
    mount_t *mnt = NULL;
    vfs_umount_plan_t plan;
    int r = vfs_umount_plan_build(path, 0, 0, &mnt, &plan);
    if (r < 0)
        return r;
    return vfs_umount_execute_plan(mnt, &plan);
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
    size_t old_size = vf->vnode ? vf->vnode->size : 0;
    if (!vfs_should_write(vf->flags)) r = -EINVAL;
    else if (!vf->vnode || !vf->vnode->ops || !vf->vnode->ops->truncate) r = -EINVAL;
    else if ((vf->seals & F_SEAL_SHRINK) && size < vf->vnode->size) r = -EPERM;
    else if ((vf->seals & F_SEAL_GROW) && size > vf->vnode->size) r = -EPERM;
    else if (vf->seals & F_SEAL_WRITE) r = -EPERM;
    else if (size > old_size &&
             quota_check_space(vf->vnode, size - old_size) < 0) r = -EDQUOT;
    else r = vf->vnode->ops->truncate(vf->vnode, size);
    if (r == 0) {
        page_cache_truncate(vf->vnode, size);
        if (vf->vnode && size > old_size)
            quota_account_space(vf->vnode, size - old_size);
    }
    vfs_put_file_ref(fd, vf);
    return r;
}
