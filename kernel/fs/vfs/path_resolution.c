/*
 * A20OS — VFS path resolution
 *
 * This file was mechanically extracted from vfs.c. It resolves paths to vnodes.
 */
#include "fs/vfs.h"
#include "fs/vfs/dcache.h"
#include "fs/vfs/path.h"
#include "fs/vfs/stat_perm.h"
#include "fs/vfs/mount.h"
#include "fs/file.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/klog.h"

extern int g_lookup_errno;

/* ============================================================
 * VFS path resolution → vnode
 * ============================================================ */



/* Resolve an absolute path within a vnode tree */
vnode_t *vnode_lookup_path(vnode_t *root, const char *path) {
    if (!root) return NULL;
    g_lookup_errno = 0;

    vnode_t *cur = root;
    vnode_get(cur);

    if (!path || !*path) return cur;

    char buf[MAX_PATH_LEN];
    strncpy(buf, path, MAX_PATH_LEN - 1);
    buf[MAX_PATH_LEN - 1] = '\0';

    char *p = buf;
    while (*p == '/') p++;

    int symlink_depth = 0;

    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (*p == '\0') {
        } else if (strcmp(p, ".") == 0) {
            /* stay */
        } else if (strcmp(p, "..") == 0) {
            if (cur->parent && cur->parent != cur) {
                vnode_t *parent = cur->parent;
                vnode_get(parent);
                vnode_put(cur);
                cur = parent;
            }
        } else {
            if (strlen(p) >= MAX_NAME_LEN) {
                vnode_put(cur);
                g_lookup_errno = -ENAMETOOLONG;
                return NULL;
            }
            if (cur->type != VFS_FT_DIR || !cur->ops || !cur->ops->lookup) {
                vnode_put(cur);
                g_lookup_errno = -ENOTDIR;
                return NULL;
            }
            if (vfs_vnode_permission(cur, X_OK) < 0) {
                vnode_put(cur);
                g_lookup_errno = -EACCES;
                return NULL;
            }
            vnode_t *next = vfs_dcache_lookup(cur, p);
            if (!next) {
                int r = cur->ops->lookup(cur, p, &next);
                if (r < 0 || !next) {
                    vnode_put(cur);
                    g_lookup_errno = r < 0 ? r : -ENOENT;
                    return NULL;
                }
                vfs_dcache_insert(cur, p, next);
            }
            vnode_t *parent = cur;
            cur = next;

            if (cur->type == VFS_FT_SYMLINK) {
                if (++symlink_depth > 8) {
                    vnode_put(parent);
                    vnode_put(cur);
                    g_lookup_errno = -ELOOP;
                    return NULL;
                }
                if (!cur->ops || !cur->ops->readlink) {
                    vnode_put(parent);
                    vnode_put(cur);
                    return NULL;
                }
                char link_target[MAX_PATH_LEN];
                int len = cur->ops->readlink(cur, link_target, sizeof(link_target));
                if (len < 0) {
                    vnode_put(parent);
                    vnode_put(cur);
                    return NULL;
                }
                link_target[len] = '\0';

                char rest[MAX_PATH_LEN];
                if (sep) {
                    snprintf(rest, sizeof(rest), "%s/%s", link_target, sep + 1);
                } else {
                    strncpy(rest, link_target, sizeof(rest) - 1);
                    rest[sizeof(rest) - 1] = '\0';
                }

                vnode_t *old = cur;
                if (link_target[0] == '/') {
                    cur = root;
                    vnode_get(cur);
                } else {
                    cur = parent;
                    vnode_get(cur);   /* compensate: we reuse parent, but it gets decremented below */
                }
                vnode_put(old);
                vnode_put(parent);

                strncpy(buf, rest, MAX_PATH_LEN - 1);
                buf[MAX_PATH_LEN - 1] = '\0';
                p = buf;
                while (*p == '/') p++;
                continue;
            }
            vnode_put(parent);
        }

        if (sep) p = sep + 1;
        else break;
    }
    return cur;
}

vnode_t *vfs_resolve(const char *path) {
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->fs.cwd[0]) ? cur->fs.cwd : "/";
    return vfs_resolve_at(path, cwd);
}

vnode_t *vfs_resolve_no_follow(const char *path) {
    return vfs_resolve_no_follow_final(path);
}

vnode_t *vfs_resolve_at(const char *path, const char *cwd) {
    char resolved[MAX_PATH_LEN];

    if (vfs_path_join(cwd, path, resolved, sizeof(resolved)) < 0)
        return NULL;
    if (vfs_path_normalize_absolute(resolved) < 0)
        return NULL;

    /* Find best matching mount */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) return NULL;

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    return vnode_lookup_path(mnt->root, rel);
}

