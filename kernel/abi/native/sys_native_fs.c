/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * This file is part of the mechanically split Native Phase 2 ABI.
 * See sys_phase2.c for shared helpers and forward declarations.
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/random.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/xattr.h"
#include "net/socket.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/objects.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                           uint16_t type, a20_rights_t rights,
                                           uint64_t expiry_tick, uint32_t remaining_ops,
                                           uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                          uint16_t expected_type, a20_rights_t required_rights,
                                          a20_handle_entry_t *out);
extern void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t a20_path_open_impl(const a20_path_open_args_t *kargs,
                                  a20_handle_t *out_handle);

/* ===== Path/Filesystem (0x0400) continued ===== */

int64_t sys_a20_path_create(const a20_syscall_args_t *args)
{
    a20_path_create_args_t *uargs = (a20_path_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_path_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, (const char *)kargs.path, (uint32_t)kargs.path_len) < 0)
        return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int r;
    if (kargs.type == 1)
        r = vfs_mkdir(full, (int)kargs.mode);
    else
        r = -1;

    if (r < 0) {
        int gfd = vfs_open(full, O_WRONLY | O_CREAT, (int)kargs.mode);
        if (gfd < 0) return -A20_ERR_NO_ENTRY;
        vfs_close(gfd);
    }

    a20_path_open_args_t open_args;
    memset(&open_args, 0, sizeof(open_args));
    open_args.size = sizeof(open_args);
    open_args.version = 1;
    open_args.dir = kargs.dir;
    open_args.path = kargs.path;
    open_args.path_len = kargs.path_len;
    open_args.flags = 0x2; /* O_RDWR */
    open_args.mode = kargs.mode;
    open_args.rights = A20_RIGHTS_ALL;

    a20_handle_t h;
    int64_t rc = a20_path_open_impl(&open_args, &h);
    if (rc < 0) return rc;

    kargs.out_handle = h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_path_unlink(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);
    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0)
        return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int r = vfs_unlink(full);
    if (r < 0) r = vfs_rmdir(full);
    return r;
}

int64_t sys_a20_path_rename(const a20_syscall_args_t *args)
{
    const char *uold = (const char *)A20_ARG(0);
    uint32_t old_len = (uint32_t)A20_ARG(1);
    const char *unew = (const char *)A20_ARG(2);
    uint32_t new_len = (uint32_t)A20_ARG(3);

    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (copy_path_from_user(kold, uold, old_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(knew, unew, new_len) < 0) return -A20_ERR_FAULT;

    char full_old[MAX_PATH_LEN], full_new[MAX_PATH_LEN];
    resolve_path(kold, full_old);
    resolve_path(knew, full_new);

    return vfs_rename(full_old, full_new);
}

int64_t sys_a20_path_readdir(const a20_syscall_args_t *args)
{
    a20_handle_t dir_h = (a20_handle_t)A20_ARG(0);
    void *buf = (void *)A20_ARG(1);
    size_t len = (size_t)A20_ARG(2);

    if (!buf || len == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dir_h, A20_OBJ_DIRECTORY,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    int gfd = (int)(uintptr_t)entry.object;

    char kbuf[4096];
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    int64_t n = vfs_getdents64(gfd, kbuf, len);
    if (n < 0) return -A20_ERR_IO;

    uint64_t out_off = 0;
    uint64_t in_off = 0;
    while (in_off < (uint64_t)n) {
        vfs_dirent64_t *vd = (vfs_dirent64_t *)(kbuf + in_off);
        if (vd->d_reclen == 0) break;

        a20_dirent_t out;
        out.type = vd->d_type;
        size_t name_len = strlen(vd->d_name);
        if (name_len > sizeof(out.name) - 1)
            name_len = sizeof(out.name) - 1;
        out.name_len = (uint32_t)name_len;
        memcpy(out.name, vd->d_name, name_len);
        out.name[name_len] = '\0';

        if (out_off + sizeof(out) > len) break;
        if (copy_to_user((char *)buf + out_off, &out, sizeof(out)) < 0)
            return -A20_ERR_FAULT;
        out_off += sizeof(out);
        in_off += vd->d_reclen;
    }

    return (int64_t)out_off;
}

int64_t sys_a20_path_link(const a20_syscall_args_t *args)
{
    const char *uold = (const char *)A20_ARG(0);
    uint32_t old_len = (uint32_t)A20_ARG(1);
    const char *unew = (const char *)A20_ARG(2);
    uint32_t new_len = (uint32_t)A20_ARG(3);

    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (copy_path_from_user(kold, uold, old_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(knew, unew, new_len) < 0) return -A20_ERR_FAULT;

    char full_old[MAX_PATH_LEN], full_new[MAX_PATH_LEN];
    resolve_path(kold, full_old);
    resolve_path(knew, full_new);

    return vfs_link(full_old, full_new);
}

int64_t sys_a20_path_symlink(const a20_syscall_args_t *args)
{
    const char *utarget = (const char *)A20_ARG(0);
    uint32_t tgt_len = (uint32_t)A20_ARG(1);
    const char *ulinkpath = (const char *)A20_ARG(2);
    uint32_t lnk_len = (uint32_t)A20_ARG(3);

    char ktgt[MAX_PATH_LEN], klnk[MAX_PATH_LEN];
    if (copy_path_from_user(ktgt, utarget, tgt_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(klnk, ulinkpath, lnk_len) < 0) return -A20_ERR_FAULT;

    char full_lnk[MAX_PATH_LEN];
    resolve_path(klnk, full_lnk);

    return vfs_symlink(ktgt, full_lnk);
}

int64_t sys_a20_path_readlink(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);
    char *ubuf = (char *)A20_ARG(2);
    size_t bufsz = (size_t)A20_ARG(3);

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0) return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    char kbuf[4096];
    int64_t r = vfs_readlinkat(-1, full, kbuf, bufsz < sizeof(kbuf) ? bufsz : sizeof(kbuf));
    if (r < 0) return -A20_ERR_NO_ENTRY;
    if (copy_to_user(ubuf, kbuf, (size_t)r) < 0) return -A20_ERR_FAULT;
    return r;
}

int64_t sys_a20_path_resolve(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0) return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int gfd = vfs_open(full, O_RDONLY, 0);
    if (gfd < 0) return -A20_ERR_NO_ENTRY;
    vfs_close(gfd);
    return A20_OK;
}

int64_t sys_a20_fs_stat(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_fs_stat_t *out = (a20_fs_stat_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    a20_fs_stat_t fs;
    memset(&fs, 0, sizeof(fs));

    if (entry.type == A20_OBJ_FILE || entry.type == A20_OBJ_DIRECTORY) {
        int gfd = (int)(uintptr_t)entry.object;
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (vf && vf->vnode) {
            fs.block_size = 4096;
        }
        if (vf) vfs_put_file_ref(gfd, vf);
    }

    if (copy_to_user(out, &fs, sizeof(fs)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_handle_control(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    uint32_t op = (uint32_t)A20_ARG(1);
    uint64_t arg0 = A20_ARG(2);
    uint64_t arg1 = A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    if (entry.type == A20_OBJ_FILE || entry.type == A20_OBJ_DEVICE) {
        int gfd = (int)(uintptr_t)entry.object;
        switch (op) {
        case 0: return vfs_ioctl(gfd, (unsigned long)arg0, (void *)arg1);
        case 1: return vfs_fcntl(gfd, (int)arg0, (long)arg1);
        default: return -A20_ERR_INVALID_ARGUMENT;
        }
    }

    if (entry.type == A20_OBJ_EVENT_QUEUE && op == 0) {
        a20_eventq_t *eq = (a20_eventq_t *)entry.object;
        if (arg0 == 1) {
            return a20_eventq_cancel(eq, (a20_handle_t)arg1);
        }
    }

    return -A20_ERR_INVALID_ARGUMENT;
}

int64_t sys_a20_fs_mount(const a20_syscall_args_t *args)
{
    a20_fs_mount_args_t *uargs = (a20_fs_mount_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_fs_mount_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    char ksrc[MAX_PATH_LEN], ktgt[MAX_PATH_LEN], kfs[64];
    if (copy_path_from_user(ksrc, (const char *)kargs.source, kargs.source_len) < 0)
        return -A20_ERR_FAULT;
    if (copy_path_from_user(ktgt, (const char *)kargs.target, kargs.target_len) < 0)
        return -A20_ERR_FAULT;
    if (copy_path_from_user(kfs, (const char *)kargs.fs_type, kargs.fs_type_len) < 0)
        return -A20_ERR_FAULT;

    char full_tgt[MAX_PATH_LEN];
    resolve_path(ktgt, full_tgt);

    int r = vfs_mount(ksrc[0] ? ksrc : NULL, full_tgt,
                       kfs[0] ? kfs : "ext2", (int)kargs.flags, NULL);
    if (r < 0) return -A20_ERR_IO;
    return A20_OK;
}

int64_t sys_a20_fs_umount(const a20_syscall_args_t *args)
{
    const char *utarget = (const char *)A20_ARG(0);
    uint32_t tgt_len = (uint32_t)A20_ARG(1);
    uint64_t flags = A20_ARG(2);
    (void)flags;

    char ktgt[MAX_PATH_LEN];
    if (copy_path_from_user(ktgt, utarget, tgt_len) < 0) return -A20_ERR_FAULT;

    char full_tgt[MAX_PATH_LEN];
    resolve_path(ktgt, full_tgt);

    int r = vfs_umount(full_tgt);
    if (r < 0) return -A20_ERR_IO;
    return A20_OK;
}

int64_t sys_a20_fs_sync(const a20_syscall_args_t *args)
{
    return vfs_sync();
}

