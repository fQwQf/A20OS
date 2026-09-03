#include "ipc/landlock.h"

#include "core/errno.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/*
 * Landlock LSM subset.
 *
 * Rulesets are anonymous files (fd-backed); add_rule() appends path rules;
 * restrict_self() marks the process restricted and detaches the ruleset fd
 * so it can no longer be extended.  Enforcement hooks (landlock_check_path)
 * are called by the VFS open/mkdir/unlink/rename entry points.
 */

typedef struct landlock_path_rule {
    char path[LANDLOCK_RULE_MAX_PREFIX];
    uint64_t access;
} landlock_path_rule_t;

typedef struct landlock_attr_path_beneath {
    uint64_t allowed_access;
    int32_t  parent_fd;
} landlock_attr_path_beneath_t;

static vfile_ops_t g_landlock_ops;

static void landlock_ruleset_put(landlock_ruleset_t *rs)
{
    if (rs && refcount_dec_and_test(&rs->refs))
        kfree(rs);
}

static int landlock_ruleset_from_fd(int gfd, landlock_ruleset_t **out)
{
    *out = NULL;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_landlock_ops) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }
    landlock_ruleset_t *rs = vf->priv;
    if (!rs || !rs->used || !refcount_inc_not_zero(&rs->refs)) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }
    *out = rs;
    vfs_put_file_ref(gfd, vf);
    return 0;
}

static int landlock_ruleset_close(vfile_t *vf)
{
    landlock_ruleset_t *rs = vf ? vf->priv : NULL;
    if (rs) {
        vf->priv = NULL;
        landlock_ruleset_put(rs);
    }
    return 0;
}

static vfile_ops_t g_landlock_ops = {
    .close = landlock_ruleset_close,
};

int landlock_create_ruleset(uint64_t handled_access_fs)
{
    if (handled_access_fs & ~LANDLOCK_ACCESS_FS_ALL)
        return -EINVAL;

    landlock_ruleset_t *rs = kcalloc(1, sizeof(*rs));
    vfile_t *vf = vfile_alloc();
    if (!rs || !vf) {
        if (rs) kfree(rs);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    refcount_set(&rs->refs, 1); /* anonymous fd owns the initial reference */
    rs->used = 1;
    vf->flags = O_RDONLY;
    vfile_ref_init(vf, 1);
    vf->ops = &g_landlock_ops;
    vf->priv = rs;
    return anonfd_install_vfile(vf, O_CLOEXEC);
}

int landlock_add_rule(int ruleset_fd, int rule_type, const void *attr_user,
                      unsigned flags)
{
    if (flags)
        return -EINVAL;
    if (rule_type != LANDLOCK_RULE_PATH_BENEATH)
        return -EINVAL;
    if (!attr_user)
        return -EFAULT;

    int64_t gfd = fdtable_get_current(ruleset_fd);
    if (gfd < 0)
        return -EBADF;

    landlock_ruleset_t *rs;
    int r = landlock_ruleset_from_fd((int)gfd, &rs);
    if (r < 0)
        return r;
    if (rs->restricted) {
        r = -EINVAL;
        goto out;
    }
    if (rs->nr_rules >= LANDLOCK_MAX_RULES) {
        r = -E2BIG;
        goto out;
    }

    landlock_attr_path_beneath_t attr;
    if (copy_from_user(&attr, attr_user, sizeof(attr)) < 0) {
        r = -EFAULT;
        goto out;
    }
    if (attr.allowed_access & ~LANDLOCK_ACCESS_FS_ALL) {
        r = -EINVAL;
        goto out;
    }

    /* Resolve parent_fd + (implicitly empty) path: parent_fd refers to a
     * directory vnode.  We store the directory's resolved path. */
    char full[MAX_PATH_LEN];
    if (attr.parent_fd == AT_FDCWD) {
        task_t *t = proc_current();
        const char *cwd = t && t->fs.cwd[0] ? t->fs.cwd : "/";
        strncpy(full, cwd, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    } else {
        if (vfs_dirfd_path((int)attr.parent_fd, full, sizeof(full)) < 0) {
            r = -EBADF;
            goto out;
        }
    }
    struct vnode *vn = vfs_resolve_no_follow(full);
    if (!vn) {
        r = -ENOENT;
        goto out;
    }
    if (vn->type != VFS_FT_DIR) {
        vnode_put(vn);
        r = -ENOTDIR;
        goto out;
    }
    vnode_put(vn);

    strncpy(rs->paths[rs->nr_rules], full, LANDLOCK_RULE_MAX_PREFIX - 1);
    rs->paths[rs->nr_rules][LANDLOCK_RULE_MAX_PREFIX - 1] = '\0';
    rs->access[rs->nr_rules] = attr.allowed_access;
    rs->nr_rules++;
    r = 0;
out:
    landlock_ruleset_put(rs);
    return r;
}

int landlock_restrict_self(int ruleset_fd, unsigned flags)
{
    if (flags)
        return -EINVAL;
    int64_t gfd = fdtable_get_current(ruleset_fd);
    if (gfd < 0)
        return -EBADF;

    landlock_ruleset_t *rs;
    int r = landlock_ruleset_from_fd((int)gfd, &rs);
    if (r < 0)
        return r;
    if (rs->restricted) {
        landlock_ruleset_put(rs);
        return -EINVAL;
    }

    task_t *t = proc_current();
    if (t->landlock_rulesets) {
        landlock_ruleset_put(rs);
        return -EINVAL;
    }
    /* Install the ruleset as the process's active set. */
    rs->restricted = 1;
    /* The reference returned by landlock_ruleset_from_fd becomes the task's
     * ownership.  Closing the userspace fd can now safely drop its own ref. */
    t->landlock_rulesets = rs;
    return 0;
}

static int landlock_rules_match(landlock_ruleset_t *rs, const char *path,
                                uint64_t needed)
{
    if (!rs || !path)
        return 0;
    for (int i = 0; i < rs->nr_rules; i++) {
        size_t plen = strlen(rs->paths[i]);
        if (strncmp(path, rs->paths[i], plen) == 0 &&
            (plen == 1 || path[plen] == '\0' || path[plen] == '/')) {
            /* A matching prefix allows only the granted accesses. */
            if ((needed & ~rs->access[i]) != 0)
                return -EACCES;
            return 0;
        }
    }
    /* No rule covers the path: for a restricted process, unknown paths are
     * denied only when the access bit is handled.  A20OS treats every path
     * under a handled ruleset as needing an explicit rule. */
    return -EACCES;
}

int landlock_check_path(const char *path, uint64_t needed_access)
{
    task_t *t = proc_current();
    if (!t)
        return 0;
    landlock_ruleset_t *rs = t->landlock_rulesets;
    if (!rs)
        return 0;
    return landlock_rules_match(rs, path, needed_access);
}

void landlock_release_task(struct task_t *t)
{
    if (!t)
        return;
    landlock_ruleset_t *rs = t->landlock_rulesets;
    t->landlock_rulesets = NULL;
    landlock_ruleset_put(rs);
}
