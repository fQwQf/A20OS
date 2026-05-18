#ifndef _A20_FS_H
#define _A20_FS_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline a20_status_t a20_path_open(const a20_path_open_args_t *args)
{
    return a20_syscall6(A20_SYS_path_open, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_path_create(a20_handle_t dir, const char *name,
                                            uint32_t name_len, uint32_t mode)
{
    return a20_syscall6(A20_SYS_path_create, dir,
                        (uint64_t)name, name_len, mode, 0, 0);
}

static inline a20_status_t a20_path_unlink(a20_handle_t dir, const char *name,
                                            uint32_t name_len)
{
    return a20_syscall6(A20_SYS_path_unlink, dir,
                        (uint64_t)name, name_len, 0, 0, 0);
}

static inline a20_status_t a20_path_rename(a20_handle_t old_dir, const char *old_name,
                                            uint32_t old_len,
                                            a20_handle_t new_dir, const char *new_name,
                                            uint32_t new_len)
{
    return a20_syscall6(A20_SYS_path_rename, old_dir,
                        (uint64_t)old_name, old_len,
                        (uint64_t)new_dir, (uint64_t)new_name, new_len);
}

static inline a20_status_t a20_path_link(a20_handle_t src_dir, const char *src_name,
                                          uint32_t src_len,
                                          a20_handle_t dst_dir, const char *dst_name,
                                          uint32_t dst_len)
{
    return a20_syscall6(A20_SYS_path_link, src_dir,
                        (uint64_t)src_name, src_len,
                        (uint64_t)dst_dir, (uint64_t)dst_name, dst_len);
}

static inline a20_status_t a20_path_symlink(a20_handle_t dir, const char *name,
                                             uint32_t name_len, const char *target,
                                             uint32_t target_len)
{
    return a20_syscall6(A20_SYS_path_symlink, dir,
                        (uint64_t)name, name_len,
                        (uint64_t)target, target_len, 0);
}

static inline a20_status_t a20_path_readlink(a20_handle_t h, char *buf,
                                              uint64_t buf_len, uint64_t *out_len)
{
    return a20_syscall6(A20_SYS_path_readlink, h,
                        (uint64_t)buf, buf_len, (uint64_t)out_len, 0, 0);
}

typedef struct {
    uint32_t type;
    uint32_t name_len;
    char     name[256];
} a20_dirent_t;

static inline a20_status_t a20_path_readdir(a20_handle_t dir, a20_dirent_t *entries,
                                             uint32_t count, uint32_t *out_count)
{
    return a20_syscall6(A20_SYS_path_readdir, dir,
                        (uint64_t)entries, count, (uint64_t)out_count, 0, 0);
}

static inline a20_status_t a20_fs_stat(a20_handle_t dir, a20_fs_stat_t *out)
{
    return a20_syscall6(A20_SYS_fs_stat, dir, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_fs_mount(a20_handle_t device, a20_handle_t mount_point,
                                         const char *fs_type, uint32_t fs_type_len,
                                         uint32_t flags)
{
    return a20_syscall6(A20_SYS_fs_mount, device, mount_point,
                        (uint64_t)fs_type, fs_type_len, flags, 0);
}

static inline a20_status_t a20_fs_umount(a20_handle_t mount_point)
{
    return a20_syscall6(A20_SYS_fs_umount, mount_point, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_fs_sync(a20_handle_t handle)
{
    return a20_syscall6(A20_SYS_fs_sync, handle, 0, 0, 0, 0, 0);
}

#endif
