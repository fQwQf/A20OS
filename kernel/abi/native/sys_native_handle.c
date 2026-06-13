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
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

/* ===== Handle (0x0100) continued ===== */

int64_t sys_a20_handle_transfer(const a20_syscall_args_t *args)
{
    a20_transfer_args_t *uargs = (a20_transfer_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_transfer_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    if (kargs.flags & ~A20_TRANSFER_PEEK)
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t src, dst;
    int64_t r;
    r = a20_handle_lookup_internal(ht, kargs.source, A20_OBJ_INVALID,
                                   A20_RIGHT_READ | A20_RIGHT_TRANSFER, &src);
    if (r < 0) return r;
    r = a20_handle_lookup_internal(ht, kargs.dest, A20_OBJ_INVALID,
                                   A20_RIGHT_WRITE | A20_RIGHT_TRANSFER, &dst);
    if (r < 0) return r;

    /* Bell-LaPadula: read from src (No Read Up) + write to dst (No Write Down) */
    uint8_t plabel = a20_ht_get_label(ht);
    if (plabel < src.security_label) return -A20_ERR_ACCESS;
    if (plabel > dst.security_label) return -A20_ERR_ACCESS;

    if (src.type != A20_OBJ_FILE && src.type != A20_OBJ_DEVICE)
        return -A20_ERR_INVALID_ARGUMENT;
    if (dst.type != A20_OBJ_FILE && dst.type != A20_OBJ_DEVICE)
        return -A20_ERR_INVALID_ARGUMENT;

    int src_gfd = (int)(uintptr_t)src.object;
    int dst_gfd = (int)(uintptr_t)dst.object;

    if (src_gfd == dst_gfd)
        return -A20_ERR_INVALID_ARGUMENT;

    long src_orig = -1;
    if (kargs.flags & A20_TRANSFER_PEEK) {
        src_orig = vfs_lseek(src_gfd, 0, SEEK_CUR);
        if (src_orig < 0) return -A20_ERR_INVALID_ARGUMENT;
    }

    if (kargs.source_offset != A20_OFFSET_CURRENT) {
        long off = vfs_lseek(src_gfd, (long)kargs.source_offset, SEEK_SET);
        if (off < 0) return -A20_ERR_INVALID_ARGUMENT;
    }

    long dst_orig = -1;
    if (kargs.dest_offset != A20_OFFSET_CURRENT) {
        dst_orig = vfs_lseek(dst_gfd, 0, SEEK_CUR);
        if (dst_orig < 0) return -A20_ERR_INVALID_ARGUMENT;
        if (vfs_lseek(dst_gfd, (long)kargs.dest_offset, SEEK_SET) < 0) {
            vfs_lseek(dst_gfd, dst_orig, SEEK_SET);
            return -A20_ERR_INVALID_ARGUMENT;
        }
    }

    char buf[4096];
    uint64_t total = 0;
    uint64_t len = kargs.length;
    while (total < len) {
        uint64_t chunk = len - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        vfile_t *svf = vfs_get_file_ref(src_gfd);
        if (!svf) break;
        int64_t n = vfs_read_file(svf, buf, (size_t)chunk);
        vfs_put_file_ref(src_gfd, svf);
        if (n <= 0) break;
        vfile_t *dvf = vfs_get_file_ref(dst_gfd);
        if (!dvf) break;
        int64_t wn = vfs_write_file(dvf, buf, (size_t)n);
        vfs_put_file_ref(dst_gfd, dvf);
        if (wn <= 0) break;
        total += (uint64_t)wn;
        if (wn < n) break;
    }

    if ((kargs.flags & A20_TRANSFER_PEEK) && src_orig >= 0)
        vfs_lseek(src_gfd, src_orig, SEEK_SET);

    kargs.out_transferred = total;
    if (copy_to_user(&uargs->out_transferred, &kargs.out_transferred,
                     sizeof(kargs.out_transferred)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)total;
}

int64_t sys_a20_handle_set_meta(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    uint32_t flags = (uint32_t)A20_ARG(1);
    uint64_t val0 = A20_ARG(2);
    uint64_t val1 = A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_WRITE | A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    if (entry.type != A20_OBJ_FILE && entry.type != A20_OBJ_DIRECTORY)
        return -A20_ERR_INVALID_ARGUMENT;

    int gfd = (int)(uintptr_t)entry.object;

    if (flags & A20_SET_META_MODE) {
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (vf && vf->vnode)
            vf->vnode->mode = (vf->vnode->mode & ~07777u) | ((uint32_t)val0 & 07777u);
        if (vf) vfs_put_file_ref(gfd, vf);
    }
    if (flags & A20_SET_META_OWNER) {
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (vf && vf->vnode) {
            vf->vnode->uid = (uint32_t)val0;
            vf->vnode->gid = (uint32_t)val1;
        }
        if (vf) vfs_put_file_ref(gfd, vf);
    }
    if (flags & (A20_SET_META_ATIME | A20_SET_META_MTIME | A20_SET_META_CTIME |
                 A20_SET_META_TRUNCATE | A20_SET_META_ALLOCATE)) {
        (void)val0; (void)val1;
    }
    return A20_OK;
}

static int64_t xattr_common(a20_handle_t h, const char *name, void *buf,
                            size_t size, uint32_t op)
{
    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    vfile_t *vf = vfs_get_file_ref((int)(uintptr_t)entry.object);
    if (!vf || !vf->vnode) {
        if (vf) vfs_put_file_ref((int)(uintptr_t)entry.object, vf);
        return -A20_ERR_BAD_HANDLE;
    }
    vnode_t *vn = vf->vnode;
    int64_t ret;
    switch (op) {
    case 0: ret = xattr_set_vnode(vn, name, buf, size, 0); break;
    case 1: ret = xattr_get_vnode(vn, name, buf, size); break;
    case 2: ret = xattr_list_vnode(vn, (char *)buf, size); break;
    case 3: ret = xattr_remove_vnode(vn, name); break;
    default: ret = -A20_ERR_INVALID_ARGUMENT; break;
    }
    vfs_put_file_ref((int)(uintptr_t)entry.object, vf);
    return ret;
}

int64_t sys_a20_handle_xattr_set(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const char *name = (const char *)A20_ARG(1);
    void *value = (void *)A20_ARG(2);
    size_t size = (size_t)A20_ARG(3);
    return xattr_common(h, name, value, size, 0);
}

int64_t sys_a20_handle_xattr_get(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const char *name = (const char *)A20_ARG(1);
    void *value = (void *)A20_ARG(2);
    size_t size = (size_t)A20_ARG(3);
    return xattr_common(h, name, value, size, 1);
}

int64_t sys_a20_handle_xattr_list(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    void *list = (void *)A20_ARG(1);
    size_t size = (size_t)A20_ARG(2);
    return xattr_common(h, NULL, list, size, 2);
}

int64_t sys_a20_handle_xattr_remove(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const char *name = (const char *)A20_ARG(1);
    return xattr_common(h, name, NULL, 0, 3);
}

