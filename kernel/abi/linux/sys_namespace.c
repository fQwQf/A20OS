#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

/* LINUX_ABI_NAMESPACE_BOUNDARY: chroot/execveat operate on current task/fs
 * state (root_path/cwd credentials).  Mount namespaces have a real object
 * model in kernel/fs/vfs/mntns.c (see unshare/setns/clone CLONE_NEWNS in
 * sys_proc.c); the remaining namespace types are system-wide singletons. */

int64_t sys_execveat(int dirfd, const char *path, char **argv, char **envp, int flags)
{
    if (!path) return -EFAULT;
    if (flags & ~1) return -EINVAL;

    char kpath[MAX_PATH_LEN];
    long pr0 = user_path_strncpy(kpath, path, MAX_PATH_LEN);
    if (pr0 < 0) return pr0;

    if (dirfd != AT_FDCWD && kpath[0] != '/') {
        char full[MAX_PATH_LEN];
        int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
        if (pr < 0) return pr;
        return abi_core_proc_exec(full, argv, envp);
    }

    return abi_core_proc_exec(kpath, argv, envp);
}

int64_t sys_chroot(const char *path)
{
    if (!path) return -EFAULT;
    task_t *cur = proc_current();
    char kpath[MAX_PATH_LEN];
    long copied = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (copied < 0) return -EFAULT;
    if (copied >= MAX_PATH_LEN - 1) return -ENAMETOOLONG;
    if (kpath[0] == '\0') return -ENOENT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(AT_FDCWD, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    kstat_t st;
    int sr = vfs_fstatat(AT_FDCWD, full, &st, 0);
    if (sr < 0) return sr;
    if ((st.st_mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
    /* Check search (execute) permission on the target directory */
    if (vfs_faccessat2(AT_FDCWD, full, X_OK, 0) < 0) return -EACCES;
    if (!proc_has_cap(cur, CAP_SYS_CHROOT)) return -EPERM;

    /* Take the root as objects, not as a string: the directory the process
     * will be confined to and the mount that backs it.  vfs_task_root_set()
     * owns both references and resets the cwd to the new root, which is
     * what Linux does. */
    vnode_t *vn = vfs_resolve(full);
    if (!vn) return vfs_lookup_errno() ? vfs_lookup_errno() : -ENOENT;
    char root[MAX_PATH_LEN];
    strncpy(root, full, sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/')
        root[--len] = '\0';
    vfs_task_root_set(cur, vn->mnt, vn, root);
    vnode_put(vn);
    /* The native-ABI shadow follows the Linux root so setns-style namespace
     * handles describe the same tree. */
    strncpy(cur->ns_ctx.fs_root, root, MAX_PATH_LEN - 1);
    cur->ns_ctx.fs_root[MAX_PATH_LEN - 1] = '\0';
    return 0;
}

int64_t sys_mknod(const char *path, int mode, unsigned dev)
{
    return sys_mknodat(AT_FDCWD, path, mode, dev);
}

int64_t sys_mknodat(int dirfd, const char *path, int mode, unsigned dev)
{
    (void)dev;
    if ((mode & S_IFMT) == S_IFDIR) return sys_mkdirat(dirfd, path, mode & 07777);
    int64_t fd = sys_openat(dirfd, path, O_CREAT | O_EXCL | O_RDWR, mode);
    if (fd < 0) return fd;
    sys_close((int)fd);
    return 0;
}
