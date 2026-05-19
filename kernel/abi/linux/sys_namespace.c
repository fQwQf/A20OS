#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

int64_t sys_execveat(int dirfd, const char *path, char **argv, char **envp, int flags)
{
    (void)dirfd; (void)flags;
    return sys_execve(path, argv, envp);
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
    strncpy(cur->fs.root_path, full, MAX_PATH_LEN - 1);
    cur->fs.root_path[MAX_PATH_LEN - 1] = '\0';
    size_t len = strlen(cur->fs.root_path);
    while (len > 1 && cur->fs.root_path[len - 1] == '/')
        cur->fs.root_path[--len] = '\0';
    cur->fs.cwd[0] = '/';
    cur->fs.cwd[1] = '\0';
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
    int64_t fd = sys_openat(dirfd, path, O_CREAT | O_EXCL | O_RDWR, mode & 07777);
    if (fd < 0) return fd;
    sys_close((int)fd);
    return 0;
}
