/* Shared Native file syscall validation, rights and I/O on all profiles. */
#if !defined(CONFIG_MCU) || defined(CONFIG_NATIVE_FLAT_FILES)
#include "core/types.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "mm/slab.h"
#include "ipc/handle_table.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/linux/fcntl.h"
#include "abi/linux/errno.h"
#include "sys_validate.h"
#ifdef CONFIG_NATIVE_FLAT_FILES
#include "fs/flat_files.h"
#endif
#define A20_ARG(n) (args->arg[(n)])
extern int64_t a20_native_vfs_result(int r);

int64_t sys_a20_handle_seek(const a20_syscall_args_t *args) {
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_off_t *offset_ptr = (a20_off_t *)(uintptr_t)A20_ARG(1);
    uint32_t whence = (uint32_t)A20_ARG(2);

    if (!offset_ptr)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_INVALID,
                                                 A20_RIGHT_SEEK, &entry);
    if (ret < 0)
        return ret;

    /* Only file-type handles support seeking */
    if (entry.type != A20_OBJ_FILE && entry.type != A20_OBJ_DEVICE) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_INVALID_ARGUMENT;
    }

    a20_off_t cur_offset;
    if (copy_from_user(&cur_offset, offset_ptr, sizeof(cur_offset)) < 0) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_FAULT;
    }

#ifdef CONFIG_NATIVE_FLAT_FILES
    if ((a20_off_t)(long)cur_offset != cur_offset) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_INVALID_ARGUMENT;
    }
#endif
    long new_off =
        vfs_lseek_vfile((vfile_t *)entry.object, (long)cur_offset, (int)whence);
    if (new_off < 0) {
        a20_object_release(entry.object, entry.type);
        /* Non-seekable objects (console, pipe, socket) report a distinct
         * error so the libc layer can map it to POSIX ESPIPE. */
        if (new_off == -ESPIPE)
            return -A20_ERR_NOT_SUPPORTED;
        return -A20_ERR_INVALID_ARGUMENT;
    }

    cur_offset = (a20_off_t)new_off;
    a20_object_release(entry.object, entry.type);

    if (copy_to_user(offset_ptr, &cur_offset, sizeof(cur_offset)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

/* ===== Path / Filesystem (0x0400) ===== */

int64_t a20_dir_handle_to_dirfd(a20_handle_t dir_h,
                                a20_rights_t required_rights, int *out_dirfd) {
    if (dir_h == A20_HANDLE_NULL) {
        *out_dirfd = AT_FDCWD;
        return 0;
    }

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, dir_h, A20_OBJ_DIRECTORY,
                                               required_rights, &entry);
    if (r < 0)
        return r;

    vfile_t *vf = (vfile_t *)entry.object;
    int lfd = fdtable_install_current_vfile(vf, 0);
    /* On success the install consumes the lookup reference; on failure the
     * lookup reference is released here. */
    if (lfd < 0) {
        a20_object_release(vf, A20_OBJ_DIRECTORY);
        return -A20_ERR_BAD_HANDLE;
    }
    *out_dirfd = lfd;
    return 0;
}

int64_t a20_install_gfd_handle(int gfd, uint16_t obj_type_hint,
                               a20_rights_t requested_rights,
                               a20_handle_t *out_handle) {
    if (gfd < 0)
        return -A20_ERR_NOT_FOUND;

    /* @gfd is the calling task's fd; the handle takes over the reference
     * resolved here (the fd slot itself is not closed — the caller keeps
     * its own fd).  The vfile outlives both. */
    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (!vf)
        return -A20_ERR_BAD_HANDLE;

    uint16_t obj_type = obj_type_hint;
    if (obj_type == A20_OBJ_INVALID) {
        obj_type = A20_OBJ_FILE;
        if (vf->vnode && vf->vnode->type == VFS_FT_DIR)
            obj_type = A20_OBJ_DIRECTORY;
    }

    a20_rights_t rights = requested_rights;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        vfs_put_file(vf);
        return -A20_ERR_BAD_HANDLE;
    }

    int64_t h = a20_handle_install(ht, vf, obj_type, rights);
    if (h < 0) {
        vfs_put_file(vf);
        return h;
    }

    if (out_handle)
        *out_handle = (a20_handle_t)h;
    return A20_OK;
}

int64_t a20_path_open_impl(const a20_path_open_args_t *kargs,
                           a20_handle_t *out_handle) {
    /* Get path string from user */
    char kpath[MAX_PATH_LEN];
    const char *upath = (const char *)(uintptr_t)kargs->path;
    if (!upath)
        return -A20_ERR_FAULT;
    if (kargs->path_len > 0 && kargs->path_len < MAX_PATH_LEN) {
        if (copy_from_user(kpath, upath, kargs->path_len) < 0)
            return -A20_ERR_FAULT;
        kpath[kargs->path_len] = '\0';
    } else {
        /* nul-terminated — copy up to MAX_PATH_LEN */
        int i;
        for (i = 0; i < MAX_PATH_LEN - 1; i++) {
            if (copy_from_user(&kpath[i], &upath[i], 1) < 0)
                return -A20_ERR_FAULT;
            if (kpath[i] == '\0')
                break;
        }
        kpath[MAX_PATH_LEN - 1] = '\0';
    }

#ifdef CONFIG_NATIVE_FLAT_FILES
    if (kargs->dir != A20_HANDLE_NULL || kargs->flags || kargs->mode)
        return -A20_ERR_NOT_SUPPORTED;
    vfile_t *flat_file;
    int open_status = a20_flat_file_open(kpath, (int)kargs->flags, &flat_file);
    if (open_status)
        return open_status == -ENOMEM ? -A20_ERR_NO_MEMORY : -A20_ERR_NOT_FOUND;
    uint16_t obj_type = A20_OBJ_FILE;
#else
    int dirfd;
    int64_t r = a20_dir_handle_to_dirfd(kargs->dir, A20_RIGHT_READ, &dirfd);
    if (r < 0)
        return r;

    int gfd = vfs_openat2(dirfd, kpath, (int)kargs->flags, (int)kargs->mode, 0);
    if (dirfd != AT_FDCWD)
        fdtable_close_current(dirfd);
    if (gfd < 0)
        return -A20_ERR_NOT_FOUND;

    uint16_t obj_type = A20_OBJ_INVALID;
    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (vf && vf->vnode && vf->vnode->type == VFS_FT_DIR)
        obj_type = A20_OBJ_DIRECTORY;
    if (vf)
        vfs_put_file(vf);

#endif
    a20_rights_t rights = A20_RIGHT_STAT | A20_RIGHT_SEEK | A20_RIGHT_DUP |
                          A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL |
                          A20_RIGHT_MAP;
    if (!(kargs->flags & O_WRONLY))
        rights |= A20_RIGHT_READ;
    if (kargs->flags & (O_WRONLY | O_RDWR))
        rights |= A20_RIGHT_WRITE;
    if (obj_type != A20_OBJ_DIRECTORY)
        rights |=
            A20_RIGHT_EXEC; /* exec permission is gated by file mode, not open flags */
#ifdef CONFIG_NATIVE_FLAT_FILES
    rights &= A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_SEEK | A20_RIGHT_EXEC;
#endif
    if (kargs->rights != 0)
        rights = kargs->rights & rights; /* user can only restrict */

#ifdef CONFIG_NATIVE_FLAT_FILES
    /* The handle consumes the open reference; no Linux fd is published. */
    int64_t handle = a20_handle_install(task_get_a20_ht(proc_current()),
                                        flat_file, obj_type, rights);
    if (handle < 0) {
        vfs_put_file(flat_file);
        return handle;
    }
    *out_handle = (a20_handle_t)handle;
    return A20_OK;
#else
    int64_t result = a20_install_gfd_handle(gfd, obj_type, rights, out_handle);
    /* The Native handle owns a separate vfile reference. This temporary fd
     * must be closed on both success and failure, as in path_create. */
    fdtable_close_current(gfd);
    return result;
#endif
}

int64_t sys_a20_path_open(const a20_syscall_args_t *args) {
    a20_path_open_args_t *uargs = (a20_path_open_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs)
        return -A20_ERR_FAULT;

    a20_path_open_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    a20_handle_t h;
    int64_t r = a20_path_open_impl(&kargs, &h);
    if (r < 0)
        return r;

    kargs.out_handle = h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        task_t *cur = proc_current();
        struct a20_ht_internal *ht = task_get_a20_ht(cur);
        if (ht)
            a20_handle_remove(ht, h);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

/* One bounded kernel buffer keeps PIPE_BUF writes atomic and positional
 * I/O under the vfile offset lock. User copies stay outside that lock, since
 * their faults may read a mapping of the same file. */
static int64_t a20_file_io(const a20_syscall_args_t *args, int writing) {
    a20_io_args_t *user = (void *)(uintptr_t)A20_ARG(0);
    a20_io_args_t io;
    A20_VALIDATE_AND_COPY(user, io);
    if (io.iov_count > 1024 || io._pad0 || io._pad1)
        return -A20_ERR_INVALID_ARGUMENT;
    struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    a20_handle_entry_t entry;
    int64_t result = a20_handle_lookup_ref_internal(
        ht, io.handle, A20_OBJ_INVALID,
        writing ? A20_RIGHT_WRITE : A20_RIGHT_READ, &entry);
    if (result < 0)
        return result;
    if (!a20_object_is_vfile_backed(entry.type)) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_INVALID_ARGUMENT;
    }
    if (writing ? a20_ht_get_label(ht) > entry.security_label
                : a20_ht_get_label(ht) < entry.security_label) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_ACCESS;
    }
    a20_iovec_t local[8];
    a20_iovec_t *iov = local;
    char *buffer = NULL;
    if (io.iov_count > 8) {
        iov = kmalloc(io.iov_count * sizeof(*iov));
        if (!iov) {
            result = -A20_ERR_NO_MEMORY;
            goto out;
        }
    }
    if (io.iov_count && copy_from_user(iov, (void *)(uintptr_t)io.iov,
                                       io.iov_count * sizeof(*iov)) < 0) {
        result = -A20_ERR_FAULT;
        goto out;
    }
    size_t total = 0;
    for (uint32_t i = 0; i < io.iov_count; i++) {
        if (iov[i].len > SIZE_MAX - total) {
            result = -A20_ERR_INVALID_ARGUMENT;
            goto out;
        }
        total += iov[i].len;
    }
    if (total > INT64_MAX) {
        result = -A20_ERR_INVALID_ARGUMENT;
        goto out;
    }
    if (!total) {
        result = 0;
        goto publish;
    }
    if (total > 65536)
        total = 65536;
    buffer = kmalloc(total);
    if (!buffer) {
        result = -A20_ERR_NO_MEMORY;
        goto out;
    }
    size_t copied = 0;
    if (writing) {
        for (uint32_t i = 0; i < io.iov_count && copied < total; i++) {
            size_t count =
                iov[i].len < total - copied ? iov[i].len : total - copied;
            if (count &&
                copy_from_user(buffer + copied, (void *)(uintptr_t)iov[i].base,
                               count) < (long)count) {
                result = -A20_ERR_FAULT;
                goto out;
            }
            copied += count;
        }
    }
    vfile_t *vf = entry.object;
    if (io.offset != UINT64_MAX) {
        if (!(entry.rights & A20_RIGHT_SEEK)) {
            result = -A20_ERR_NOT_SUPPORTED;
            goto out;
        }
        result = writing ? vfs_pwrite_file(vf, buffer, total, io.offset)
                         : vfs_pread_file(vf, buffer, total, io.offset);
    } else {
        int lock_offset = vf && vf->vnode && vf->ops && vf->ops->lseek;
        if (lock_offset)
            mutex_lock(&vf->offset_lock);
        result = writing ? vfs_write_file(vf, buffer, total)
                         : vfs_read_file(vf, buffer, total);
        if (lock_offset)
            mutex_unlock(&vf->offset_lock);
    }
    if (result < 0) {
        result = result == -ESPIPE ? -A20_ERR_NOT_SUPPORTED
                                   : a20_native_vfs_result((int)result);
        goto out;
    }
    if ((uint64_t)result > total) {
        result = -A20_ERR_IO;
        goto out;
    }
    if (!writing) {
        for (uint32_t i = 0; i < io.iov_count && copied < (size_t)result; i++) {
            size_t count = iov[i].len < (size_t)result - copied
                               ? iov[i].len
                               : (size_t)result - copied;
            if (count && copy_to_user((void *)(uintptr_t)iov[i].base,
                                      buffer + copied, count) < (long)count) {
                if (!copied) {
                    result = -A20_ERR_FAULT;
                    goto out;
                }
                result = (int64_t)copied;
                goto publish;
            }
            copied += count;
        }
    }
publish:
    io.out_count = result;
    if (a20_copy_struct_to_user(user, &io, sizeof(io)) < 0)
        result = -A20_ERR_FAULT;
out:
    kfree(buffer);
    if (iov != local)
        kfree(iov);
    a20_object_release(entry.object, entry.type);
    return result;
}

int64_t sys_a20_handle_read(const a20_syscall_args_t *args) {
    return a20_file_io(args, 0);
}

int64_t sys_a20_handle_write(const a20_syscall_args_t *args) {
    return a20_file_io(args, 1);
}

int64_t sys_a20_handle_stat(const a20_syscall_args_t *args) {
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_stat_t *out = (a20_stat_t *)(uintptr_t)A20_ARG(1);
    if (!out)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_INVALID,
                                                 A20_RIGHT_STAT, &entry);
    if (ret < 0)
        return ret;

    /* For vfile-backed handles, use vfs_fstat */
    if (entry.type == A20_OBJ_FILE || entry.type == A20_OBJ_DIRECTORY ||
        entry.type == A20_OBJ_PIPE_ENDPOINT || entry.type == A20_OBJ_DEVICE) {
        kstat_t ks;
        int sr = vfs_vfile_stat((vfile_t *)entry.object, &ks);
        a20_object_release(entry.object, entry.type);
        if (sr < 0)
            return -A20_ERR_IO;

        a20_stat_t st;
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        st.version = 1;
        st.dev = ks.st_dev;
        st.ino = ks.st_ino;
        st.mode = ks.st_mode;
        st.nlink = ks.st_nlink;
        st.uid = ks.st_uid;
        st.gid = ks.st_gid;
        st.size_bytes = ks.st_size;
        st.blocks = ks.st_blocks;
        st.atime_ns = ks.st_atime * 1000000000ULL + ks.st_atime_nsec;
        st.mtime_ns = ks.st_mtime * 1000000000ULL + ks.st_mtime_nsec;
        st.ctime_ns = ks.st_ctime * 1000000000ULL + ks.st_ctime_nsec;

        if (copy_to_user(out, &st, sizeof(st)) < 0)
            return -A20_ERR_FAULT;
        return A20_OK;
    }

    a20_object_release(entry.object, entry.type);

    /* Non-file handles: return minimal info */
    a20_stat_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    st.version = 1;
    if (copy_to_user(out, &st, sizeof(st)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

#endif
