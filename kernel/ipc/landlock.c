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

/* Leaf lock: only bounded Rust policy reads/updates. Never hold it across
 * usercopy, VFS, allocation, reference release or task publication. */
static spinlock_t g_landlock_policy_lock = SPINLOCK_INIT;
static vfile_ops_t g_landlock_ops;
extern int a20_landlock_create(uint64_t handled, void **out);
extern bool a20_landlock_get(void *rs);
extern void a20_landlock_put(void *rs);
extern int a20_landlock_add(void *rs, const uint8_t *path, uint64_t access);
extern int a20_landlock_restrict(void *rs);
extern int a20_landlock_match(void *rs, const uint8_t *path, uint64_t needed);
void *a20_landlock_alloc(size_t size) { return kcalloc(1, size); }
void a20_landlock_free(void *ptr) { kfree(ptr); }

typedef struct __attribute__((packed)) { uint64_t allowed_access; int32_t parent_fd; } landlock_attr_path_beneath_t;
static int landlock_ruleset_close(vfile_t *vf)
{
    if (vf && vf->priv) { a20_landlock_put(vf->priv); vf->priv = NULL; }
    return 0;
}
static vfile_ops_t g_landlock_ops = { .close = landlock_ruleset_close };

static int landlock_ruleset_from_fd(int gfd, void **out)
{
    *out = NULL;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf) return -EBADF;
    void *rs = vf->ops == &g_landlock_ops ? vf->priv : NULL;
    bool got = rs && a20_landlock_get(rs);
    vfs_put_file_ref(gfd, vf);
    if (!got) return -EBADF;
    *out = rs;
    return 0;
}

int landlock_create_ruleset(uint64_t handled_access_fs)
{
    void *rs = NULL;
    int r = a20_landlock_create(handled_access_fs, &rs);
    if (r < 0) return r;
    vfile_t *vf = vfile_alloc();
    if (!vf) { a20_landlock_put(rs); return -ENOMEM; }
    vf->flags = O_RDONLY; vfile_ref_init(vf, 1); vf->ops = &g_landlock_ops; vf->priv = rs;
    return anonfd_install_vfile(vf, O_CLOEXEC);
}

int landlock_add_rule(int ruleset_fd, int rule_type, const void *attr_user, unsigned flags)
{
    if (flags || rule_type != LANDLOCK_RULE_PATH_BENEATH) return -EINVAL;
    if (!attr_user) return -EFAULT;
    int64_t gfd = fdtable_get_current(ruleset_fd);
    if (gfd < 0) return -EBADF;
    void *rs; int r = landlock_ruleset_from_fd((int)gfd, &rs); if (r < 0) return r;
    landlock_attr_path_beneath_t attr;
    if (copy_from_user(&attr, attr_user, sizeof(attr)) < 0) { r=-EFAULT; goto out; }
    char full[MAX_PATH_LEN];
    if (attr.parent_fd == AT_FDCWD) {
        task_t *t=proc_current(); const char *cwd=t && t->fs.cwd[0]?t->fs.cwd:"/";
        strncpy(full,cwd,sizeof(full)-1); full[sizeof(full)-1]='\0';
    } else if (vfs_dirfd_path(attr.parent_fd,full,sizeof(full)) < 0) { r=-EBADF; goto out; }
    char canonical[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *root = cur && cur->fs.root_path[0] ? cur->fs.root_path : "/";
    int error = 0;
    struct vnode *vn = vnode_lookup_path_openat2(full, root, root, 0,
                                                 canonical, sizeof(canonical), &error);
    if (!vn) { r = error ? error : -ENOENT; goto out; }
    if (vn->type != VFS_FT_DIR) { vnode_put(vn); r=-ENOTDIR; goto out; }
    vnode_put(vn);
    spin_lock(&g_landlock_policy_lock);
    r = a20_landlock_add(rs, (const uint8_t *)canonical, attr.allowed_access);
    spin_unlock(&g_landlock_policy_lock);
out:
    a20_landlock_put(rs); return r;
}

int landlock_restrict_self(int ruleset_fd, unsigned flags)
{
    if (flags) return -EINVAL;
    int64_t gfd=fdtable_get_current(ruleset_fd); if (gfd<0) return -EBADF;
    void *rs; int r=landlock_ruleset_from_fd((int)gfd,&rs); if (r<0) return r;
    task_t *t=proc_current();
    if (!t || t->landlock_rulesets) { a20_landlock_put(rs); return -EINVAL; }
    if (!t->policy.no_new_privs && !proc_has_cap(t, CAP_SYS_ADMIN)) {
        a20_landlock_put(rs);
        return -EACCES;
    }
    spin_lock(&g_landlock_policy_lock);
    r = a20_landlock_restrict(rs);
    spin_unlock(&g_landlock_policy_lock);
    if (r<0) { a20_landlock_put(rs); return r; }
    __atomic_store_n(&t->landlock_rulesets, rs, __ATOMIC_RELEASE); return 0;
}
void landlock_inherit_task(struct task_t *child, const struct task_t *parent)
{
    /* Task construction pins the live parent and runs before publication.
     * Only that parent installs its policy; teardown cannot race this pin. */
    void *rs = parent ? __atomic_load_n(&parent->landlock_rulesets, __ATOMIC_ACQUIRE) : NULL;
    child->landlock_rulesets = a20_landlock_get(rs) ? rs : NULL;
}

int landlock_check_path(const char *path, uint64_t needed_access)
{
    task_t *t = proc_current();
    void *rs = t ? __atomic_load_n(&t->landlock_rulesets, __ATOMIC_ACQUIRE) : NULL;
    if (!rs) return 0;
    spin_lock(&g_landlock_policy_lock);
    int r = a20_landlock_match(rs, (const uint8_t *)path, needed_access);
    spin_unlock(&g_landlock_policy_lock);
    return r;
}
void landlock_release_task(struct task_t *t)
{
    if (t)
        a20_landlock_put(__atomic_exchange_n(&t->landlock_rulesets, NULL, __ATOMIC_ACQ_REL));
}
