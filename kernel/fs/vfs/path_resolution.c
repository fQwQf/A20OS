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



static int vnode_is_mount_root(vnode_t *vn)
{
    return vn && vn->mnt && vn->mnt->root &&
           vn->type == VFS_FT_DIR &&
           vn->ino == vn->mnt->root->ino;
}

static int count_path_components(const char *path)
{
    int n = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        n++;
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
    }
    return n;
}

/* Resolve an absolute path within a vnode tree, honouring openat2 resolve
 * flags.  On success returns a referenced vnode and writes the fully-resolved
 * absolute path to resolved_out.  On failure returns NULL and sets *lookup_err.
 */
vnode_t *vnode_lookup_path_openat2(const char *path,
                                   const char *start,
                                   const char *root_path,
                                   uint64_t resolve,
                                   char *resolved_out,
                                   size_t resolved_out_sz,
                                   int *lookup_err) {
    if (!path || !start || !root_path || !resolved_out || resolved_out_sz == 0 || !lookup_err) {
        if (lookup_err) *lookup_err = -EINVAL;
        return NULL;
    }

    *lookup_err = 0;
    strncpy(resolved_out, path, resolved_out_sz - 1);
    resolved_out[resolved_out_sz - 1] = '\0';

    if (vfs_path_normalize_absolute_with_root(resolved_out, root_path) < 0) {
        *lookup_err = -ENAMETOOLONG;
        return NULL;
    }

    mount_t *start_mnt = vfs_find_mount(start);
    if (!start_mnt) start_mnt = vfs_find_mount("/");

    mount_t *mnt = vfs_find_mount(resolved_out);
    if (!mnt || !mnt->root) {
        *lookup_err = -ENOENT;
        return NULL;
    }

    if ((resolve & RESOLVE_NO_XDEV) && mnt != start_mnt) {
        *lookup_err = -EXDEV;
        return NULL;
    }

    if ((resolve & RESOLVE_BENEATH) && !path_is_beneath(start, resolved_out)) {
        *lookup_err = -EXDEV;
        return NULL;
    }

    vnode_t *cur = mnt->root;
    vnode_get(cur);

    if (strcmp(resolved_out, "/") == 0) {
        return cur;
    }

    char buf[MAX_PATH_LEN];
    char link_target[MAX_PATH_LEN];
    char cur_abs_path[MAX_PATH_LEN];
    int symlink_depth = 0;

    strncpy(cur_abs_path, start, sizeof(cur_abs_path) - 1);
    cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
    vfs_path_trim_trailing_slashes(cur_abs_path);
    if (cur_abs_path[0] == '\0') {
        cur_abs_path[0] = '/';
        cur_abs_path[1] = '\0';
    }

    const char *rel = vfs_strip_mount_prefix(resolved_out, mnt);
    strncpy(buf, rel, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p == '/') p++;

    int depth = 0;

    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (*p == '\0') {
        } else if (strcmp(p, ".") == 0) {
        } else if (strcmp(p, "..") == 0) {
            if (depth > 0 && cur->parent && cur->parent != cur) {
                vnode_t *parent = cur->parent;
                vnode_get(parent);
                vnode_put(cur);
                cur = parent;
                depth--;
                char *last = strrchr(cur_abs_path, '/');
                if (last) {
                    if (last == cur_abs_path)
                        cur_abs_path[1] = '\0';
                    else
                        *last = '\0';
                }
            } else if (vnode_is_mount_root(cur)) {
                mount_t *parent_mnt = vfs_mount_parent(cur->mnt);
                if (parent_mnt) {
                    char new_path[MAX_PATH_LEN];
                    char parent_rel[MAX_PATH_LEN];
                    const char *mp_rel = vfs_strip_mount_prefix(cur->mnt->path, parent_mnt);
                    if (strcmp(mp_rel, "") == 0 || strcmp(mp_rel, "/") == 0) {
                        parent_rel[0] = '\0';
                    } else {
                        strncpy(parent_rel, mp_rel, sizeof(parent_rel) - 1);
                        parent_rel[sizeof(parent_rel) - 1] = '\0';
                        char *s = strrchr(parent_rel, '/');
                        if (s) {
                            if (s == parent_rel)
                                parent_rel[1] = '\0';
                            else
                                *s = '\0';
                        } else {
                            parent_rel[0] = '\0';
                        }
                    }
                    vfs_path_join(parent_mnt->path, parent_rel, new_path, sizeof(new_path));
                    if (vfs_path_normalize_absolute_with_root(new_path, root_path) < 0) {
                        vnode_put(cur);
                        *lookup_err = -ENAMETOOLONG;
                        return NULL;
                    }
                    if ((resolve & RESOLVE_BENEATH) && !path_is_beneath(start, new_path)) {
                        vnode_put(cur);
                        *lookup_err = -EXDEV;
                        return NULL;
                    }
                    if ((resolve & RESOLVE_NO_XDEV) && parent_mnt != start_mnt) {
                        vnode_put(cur);
                        *lookup_err = -EXDEV;
                        return NULL;
                    }
                    vnode_put(cur);
                    cur = vnode_lookup_path(parent_mnt->root, parent_rel);
                    if (!cur) {
                        *lookup_err = g_lookup_errno ? g_lookup_errno : -ENOENT;
                        return NULL;
                    }
                    depth = count_path_components(parent_rel);
                    strncpy(cur_abs_path, new_path, sizeof(cur_abs_path) - 1);
                    cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
                }
            }
        } else {
            if (strlen(p) >= MAX_NAME_LEN) {
                vnode_put(cur);
                *lookup_err = -ENAMETOOLONG;
                return NULL;
            }
            if (cur->type != VFS_FT_DIR || !cur->ops || !cur->ops->lookup) {
                vnode_put(cur);
                *lookup_err = -ENOTDIR;
                return NULL;
            }
            if (vfs_vnode_permission(cur, X_OK) < 0) {
                vnode_put(cur);
                *lookup_err = -EACCES;
                return NULL;
            }

            vnode_t *next = vfs_dcache_lookup(cur, p);
            if (!next) {
                int r = cur->ops->lookup(cur, p, &next);
                if (r < 0 || !next) {
                    vnode_put(cur);
                    *lookup_err = r < 0 ? r : -ENOENT;
                    return NULL;
                }
                vfs_dcache_insert(cur, p, next);
            }

            vnode_t *parent = cur;
            cur = next;

            if (cur->type == VFS_FT_SYMLINK) {
                int is_final = (sep == NULL);
                int follow = 1;
                if (resolve & RESOLVE_NO_SYMLINKS) follow = 0;
                else if ((resolve & RESOLVE_NO_TRAILING_SYMLINKS) && is_final) follow = 0;

                if (!follow) {
                    vnode_put(parent);
                    vnode_put(cur);
                    *lookup_err = -ELOOP;
                    return NULL;
                }

                if (++symlink_depth > MAX_SYMLINKS) {
                    vnode_put(parent);
                    vnode_put(cur);
                    *lookup_err = -ELOOP;
                    return NULL;
                }

                if (!cur->ops || !cur->ops->readlink) {
                    vnode_put(parent);
                    vnode_put(cur);
                    *lookup_err = -ENOSYS;
                    return NULL;
                }

                int len = cur->ops->readlink(cur, link_target, sizeof(link_target));
                if (len < 0) {
                    vnode_put(parent);
                    vnode_put(cur);
                    *lookup_err = len;
                    return NULL;
                }
                link_target[len] = '\0';

                vnode_t *old = cur;
                char rest[MAX_PATH_LEN];
                if (sep) {
                    snprintf(rest, sizeof(rest), "%s/%s", link_target, sep + 1);
                } else {
                    strncpy(rest, link_target, sizeof(rest) - 1);
                    rest[sizeof(rest) - 1] = '\0';
                }

                char new_path[MAX_PATH_LEN];
                if (link_target[0] == '/') {
                    if (resolve & RESOLVE_IN_ROOT) {
                        snprintf(new_path, sizeof(new_path), "%s%s", start, link_target);
                    } else {
                        snprintf(new_path, sizeof(new_path), "%s", link_target);
                    }
                } else {
                    vfs_path_join(cur_abs_path, rest, new_path, sizeof(new_path));
                }

                if (vfs_path_normalize_absolute_with_root(new_path, root_path) < 0) {
                    vnode_put(old);
                    vnode_put(parent);
                    *lookup_err = -ENAMETOOLONG;
                    return NULL;
                }
                if ((resolve & RESOLVE_BENEATH) && !path_is_beneath(start, new_path)) {
                    vnode_put(old);
                    vnode_put(parent);
                    *lookup_err = -EXDEV;
                    return NULL;
                }
                mount_t *new_mnt = vfs_find_mount(new_path);
                if (!new_mnt || !new_mnt->root) {
                    vnode_put(old);
                    vnode_put(parent);
                    *lookup_err = -ENOENT;
                    return NULL;
                }
                if ((resolve & RESOLVE_NO_XDEV) && new_mnt != start_mnt) {
                    vnode_put(old);
                    vnode_put(parent);
                    *lookup_err = -EXDEV;
                    return NULL;
                }
                cur = new_mnt->root;
                vnode_get(cur);
                depth = 0;
                strncpy(cur_abs_path, new_path, sizeof(cur_abs_path) - 1);
                cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
                vfs_path_trim_trailing_slashes(cur_abs_path);
                if (cur_abs_path[0] == '\0') {
                    cur_abs_path[0] = '/';
                    cur_abs_path[1] = '\0';
                }
                strncpy(resolved_out, new_path, resolved_out_sz - 1);
                resolved_out[resolved_out_sz - 1] = '\0';
                const char *new_rel = vfs_strip_mount_prefix(new_path, new_mnt);
                strncpy(buf, new_rel, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';

                vnode_put(old);
                vnode_put(parent);
                p = buf;
                while (*p == '/') p++;
                continue;
            }

            if (cur->type == VFS_FT_DIR) {
                depth++;
                char tmp[MAX_PATH_LEN];
                vfs_path_join(cur_abs_path, p, tmp, sizeof(tmp));
                strncpy(cur_abs_path, tmp, sizeof(cur_abs_path) - 1);
                cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
            }
            vnode_put(parent);
        }

        if (sep) p = sep + 1;
        else break;
    }

    return cur;
}

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
    int depth = 0;

    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (*p == '\0') {
        } else if (strcmp(p, ".") == 0) {
            /* stay */
        } else if (strcmp(p, "..") == 0) {
            if (depth > 0 && cur->parent && cur->parent != cur) {
                vnode_t *parent = cur->parent;
                vnode_get(parent);
                vnode_put(cur);
                cur = parent;
                depth--;
            } else if (vnode_is_mount_root(cur)) {
                mount_t *parent_mnt = vfs_mount_parent(cur->mnt);
                if (parent_mnt) {
                    const char *mp_rel = vfs_strip_mount_prefix(cur->mnt->path, parent_mnt);
                    char parent_rel[MAX_PATH_LEN];
                    if (strcmp(mp_rel, "") == 0 || strcmp(mp_rel, "/") == 0) {
                        parent_rel[0] = '\0';
                    } else {
                        strncpy(parent_rel, mp_rel, sizeof(parent_rel) - 1);
                        parent_rel[sizeof(parent_rel) - 1] = '\0';
                        char *s = strrchr(parent_rel, '/');
                        if (s) {
                            if (s == parent_rel)
                                parent_rel[1] = '\0';
                            else
                                *s = '\0';
                        } else {
                            parent_rel[0] = '\0';
                        }
                    }
                    vnode_put(cur);
                    cur = vnode_lookup_path(parent_mnt->root, parent_rel);
                    if (!cur) return NULL;
                    depth = count_path_components(parent_rel);
                }
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
                if (++symlink_depth > MAX_SYMLINKS) {
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
                    depth = 0;
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
            if (cur->type == VFS_FT_DIR)
                depth++;
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

    task_t *cur = proc_current();
    const char *root = (cur && cur->fs.root_path[0]) ? cur->fs.root_path : "/";
    if (strcmp(root, "/") != 0) {
        char rooted[MAX_PATH_LEN];
        if (strcmp(resolved, "/") == 0)
            snprintf(rooted, sizeof(rooted), "%s", root);
        else
            snprintf(rooted, sizeof(rooted), "%s%s", root, resolved);
        strncpy(resolved, rooted, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    if (vfs_path_normalize_absolute_with_root(resolved, root) < 0)
        return NULL;

    /* Find best matching mount */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) return NULL;

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    return vnode_lookup_path(mnt->root, rel);
}

