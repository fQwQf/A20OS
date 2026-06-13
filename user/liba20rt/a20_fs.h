#ifndef _A20_FS_H
#define _A20_FS_H

#include "a20_types.h"
#include "a20_syscall.h"

/* Native ABI path_open flags (match Linux open(2) values used by VFS) */
#define A20_PATH_OPEN_RDONLY   0x00000000u
#define A20_PATH_OPEN_WRONLY   0x00000001u
#define A20_PATH_OPEN_RDWR     0x00000002u
#define A20_PATH_OPEN_CREATE   0x00000040u
#define A20_PATH_OPEN_TRUNC    0x00000200u
#define A20_PATH_OPEN_APPEND   0x00000400u

static inline a20_status_t a20_path_open(const a20_path_open_args_t *args)
{
    return a20_syscall6(A20_SYS_path_open, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_path_create(const a20_path_create_args_t *args)
{
    return a20_syscall6(A20_SYS_path_create, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_path_unlink(a20_handle_t dir, const char *name,
                                            uint32_t name_len)
{
    /* Kernel path_unlink currently resolves 'name' against cwd, not 'dir'. */
    (void)dir;
    return a20_syscall6(A20_SYS_path_unlink,
                        (uint64_t)name, name_len, 0, 0, 0, 0);
}

static inline a20_status_t a20_path_rename(const char *old_path, uint32_t old_len,
                                            const char *new_path, uint32_t new_len)
{
    return a20_syscall6(A20_SYS_path_rename,
                        (uint64_t)old_path, old_len,
                        (uint64_t)new_path, new_len, 0, 0);
}

static inline a20_status_t a20_path_link(const char *old_path, uint32_t old_len,
                                          const char *new_path, uint32_t new_len)
{
    return a20_syscall6(A20_SYS_path_link,
                        (uint64_t)old_path, old_len,
                        (uint64_t)new_path, new_len, 0, 0);
}

static inline a20_status_t a20_path_symlink(const char *target, uint32_t target_len,
                                             const char *linkpath, uint32_t linkpath_len)
{
    return a20_syscall6(A20_SYS_path_symlink,
                        (uint64_t)target, target_len,
                        (uint64_t)linkpath, linkpath_len, 0, 0);
}

static inline int64_t a20_path_readlink(const char *path, uint32_t path_len,
                                         char *buf, uint64_t buf_len)
{
    return a20_syscall6(A20_SYS_path_readlink,
                        (uint64_t)path, path_len,
                        (uint64_t)buf, buf_len, 0, 0);
}

static inline int64_t a20_path_readdir(a20_handle_t dir, a20_dirent_t *entries,
                                        uint32_t count)
{
    return a20_syscall6(A20_SYS_path_readdir, dir,
                        (uint64_t)entries,
                        (uint64_t)count * sizeof(a20_dirent_t), 0, 0, 0);
}

static inline a20_status_t a20_fs_stat(a20_handle_t h, a20_fs_stat_t *out)
{
    return a20_syscall6(A20_SYS_fs_stat, h, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_fs_mount(const a20_fs_mount_args_t *args)
{
    return a20_syscall6(A20_SYS_fs_mount, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_fs_umount(const char *target, uint32_t target_len,
                                          uint32_t flags)
{
    return a20_syscall6(A20_SYS_fs_umount, (uint64_t)target, target_len, flags,
                        0, 0, 0);
}

static inline a20_status_t a20_fs_sync(void)
{
    return a20_syscall6(A20_SYS_fs_sync, 0, 0, 0, 0, 0, 0);
}

#endif
