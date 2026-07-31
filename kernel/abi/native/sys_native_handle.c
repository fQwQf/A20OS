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
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/linux/poll.h"
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
extern int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                               a20_handle_t h,
                                               uint16_t expected_type,
                                               a20_rights_t required_rights,
                                               a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern void a20_object_release(void *object, uint16_t type);

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
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags & ~A20_TRANSFER_PEEK)
        return -A20_ERR_INVALID_ARGUMENT;

    const uint64_t a20_off_max = ~(1ULL << 63);
    if (kargs.source_offset != A20_OFFSET_CURRENT &&
        kargs.source_offset > a20_off_max)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.dest_offset != A20_OFFSET_CURRENT &&
        kargs.dest_offset > a20_off_max)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.length > 0) {
        if (kargs.source_offset != A20_OFFSET_CURRENT &&
            kargs.source_offset > a20_off_max - kargs.length)
            return -A20_ERR_INVALID_ARGUMENT;
        if (kargs.dest_offset != A20_OFFSET_CURRENT &&
            kargs.dest_offset > a20_off_max - kargs.length)
            return -A20_ERR_INVALID_ARGUMENT;
    }

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t src, dst;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.source,
                                               A20_OBJ_INVALID,
                                               A20_RIGHT_READ | A20_RIGHT_TRANSFER,
                                               &src);
    if (r < 0) return r;
    r = a20_handle_lookup_ref_internal(ht, kargs.dest,
                                       A20_OBJ_INVALID,
                                       A20_RIGHT_WRITE | A20_RIGHT_TRANSFER,
                                       &dst);
    if (r < 0) goto out_src;

    /* Bell-LaPadula: read from src (No Read Up) + write to dst (No Write Down) */
    uint8_t plabel = a20_ht_get_label(ht);
    if (plabel < src.security_label || plabel > dst.security_label) {
        r = -A20_ERR_ACCESS;
        goto out_both;
    }

    if ((src.type != A20_OBJ_FILE && src.type != A20_OBJ_DEVICE) ||
        (dst.type != A20_OBJ_FILE && dst.type != A20_OBJ_DEVICE)) {
        r = -A20_ERR_INVALID_ARGUMENT;
        goto out_both;
    }

    int src_gfd = (int)(uintptr_t)src.object;
    int dst_gfd = (int)(uintptr_t)dst.object;
    if (src_gfd == dst_gfd) {
        r = -A20_ERR_INVALID_ARGUMENT;
        goto out_both;
    }

    long src_orig = -1;
    if (kargs.flags & A20_TRANSFER_PEEK) {
        src_orig = vfs_lseek(src_gfd, 0, SEEK_CUR);
        if (src_orig < 0) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_both;
        }
    }

    if (kargs.source_offset != A20_OFFSET_CURRENT) {
        long off = vfs_lseek(src_gfd, (long)kargs.source_offset, SEEK_SET);
        if (off < 0) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_both;
        }
    }

    long dst_orig = -1;
    if (kargs.dest_offset != A20_OFFSET_CURRENT) {
        dst_orig = vfs_lseek(dst_gfd, 0, SEEK_CUR);
        if (dst_orig < 0) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_both;
        }
        if (vfs_lseek(dst_gfd, (long)kargs.dest_offset, SEEK_SET) < 0) {
            vfs_lseek(dst_gfd, dst_orig, SEEK_SET);
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_both;
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
    a20_object_release(dst.object, dst.type);
    a20_object_release(src.object, src.type);
    if (copy_to_user(&uargs->out_transferred, &kargs.out_transferred,
                     sizeof(kargs.out_transferred)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)total;

out_both:
    a20_object_release(dst.object, dst.type);
out_src:
    a20_object_release(src.object, src.type);
    return r;
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
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_INVALID,
                                               A20_RIGHT_WRITE | A20_RIGHT_STAT,
                                               &entry);
    if (r < 0) return r;

    if (entry.type != A20_OBJ_FILE && entry.type != A20_OBJ_DIRECTORY) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_INVALID_ARGUMENT;
    }


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
    a20_object_release(entry.object, entry.type);
    return A20_OK;
}

#define A20_XATTR_LIST_MAX (1024 * XATTR_NAME_MAX_LOCAL)

static int64_t xattr_common(a20_handle_t h, const char *name, void *buf,
                            size_t size, uint32_t op)
{
    char kname[XATTR_NAME_MAX_LOCAL];
    const char *knamep = NULL;
    if (op != 2) {
        if (!name) return -A20_ERR_INVALID_ARGUMENT;
        long nr = user_strncpy(kname, name, sizeof(kname));
        if (nr < 0) return -A20_ERR_FAULT;
        knamep = kname;
    }

    uint8_t kvalue[XATTR_VALUE_MAX_LOCAL];
    void *kbuf = NULL;
    size_t kbuf_size = 0;
    if (op == 0) {
        if (size > sizeof(kvalue)) return -A20_ERR_NO_SPACE;
        if (size > 0) {
            if (!buf) return -A20_ERR_FAULT;
            if (copy_from_user(kvalue, buf, size) < 0)
                return -A20_ERR_FAULT;
        }
    } else if (op == 2 && buf && size > 0) {
        kbuf_size = size > A20_XATTR_LIST_MAX ? A20_XATTR_LIST_MAX : size;
        kbuf = kmalloc(kbuf_size);
        if (!kbuf) return -A20_ERR_NO_MEMORY;
    }

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        kfree(kbuf);
        return -A20_ERR_BAD_HANDLE;
    }

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_INVALID,
                                               A20_RIGHT_STAT, &entry);
    if (r < 0) goto out_free;
    if (entry.type != A20_OBJ_FILE && entry.type != A20_OBJ_DIRECTORY &&
        entry.type != A20_OBJ_DEVICE) {
        r = -A20_ERR_INVALID_ARGUMENT;
        goto out_entry;
    }

    vfile_t *vf = vfs_get_file_ref((int)(uintptr_t)entry.object);
    if (!vf || !vf->vnode) {
        if (vf) vfs_put_file_ref((int)(uintptr_t)entry.object, vf);
        r = -A20_ERR_BAD_HANDLE;
        goto out_entry;
    }
    vnode_t *vn = vf->vnode;
    switch (op) {
    case 0:
        r = xattr_set_vnode(vn, knamep, kvalue, size, 0);
        break;
    case 1:
        if (!buf || size == 0) {
            r = xattr_get_vnode(vn, knamep, NULL, 0);
        } else {
            size_t get_size = size < sizeof(kvalue) ? size : sizeof(kvalue);
            r = xattr_get_vnode(vn, knamep, kvalue, get_size);
        }
        break;
    case 2:
        r = xattr_list_vnode(vn, (char *)kbuf, kbuf_size);
        break;
    case 3:
        r = xattr_remove_vnode(vn, knamep);
        break;
    default:
        r = -A20_ERR_INVALID_ARGUMENT;
        break;
    }
    vfs_put_file_ref((int)(uintptr_t)entry.object, vf);
    a20_object_release(entry.object, entry.type);

    if (r >= 0 && op == 1 && buf && size > 0 && r > 0 &&
        copy_to_user(buf, kvalue, (size_t)r) < 0)
        r = -A20_ERR_FAULT;
    if (r >= 0 && op == 2 && kbuf && r > 0 &&
        copy_to_user(buf, kbuf, (size_t)r) < 0)
        r = -A20_ERR_FAULT;
    kfree(kbuf);
    return r;

out_entry:
    a20_object_release(entry.object, entry.type);
out_free:
    kfree(kbuf);
    return r;
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


/* ===== handle_poll (0x010C) =====
 * Non-blocking readiness query.  Blocking waits belong to event_queue;
 * this syscall only reports the current level, reusing vfs_poll_events for
 * vfile-backed objects (file/dir/pipe/device/socket). */

extern int vfs_poll_events(int fd, short events);

int64_t sys_a20_handle_poll(const a20_syscall_args_t *args)
{
    a20_handle_poll_args_t *uargs = (a20_handle_poll_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_handle_poll_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags != 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.handle,
                                               A20_OBJ_INVALID, 0, &entry);
    if (r < 0) return r;

    uint64_t active = 0;
    switch (entry.type) {
    case A20_OBJ_FILE:
    case A20_OBJ_DIRECTORY:
    case A20_OBJ_PIPE_ENDPOINT:
    case A20_OBJ_DEVICE:
    case A20_OBJ_SOCKET: {
        int gfd = (int)(uintptr_t)entry.object;
        int rev = vfs_poll_events(gfd, POLLIN | POLLOUT);
        if (rev < 0) {
            a20_object_release(entry.object, entry.type);
            return -A20_ERR_IO;
        }
        if (rev & (POLLIN | POLLPRI))
            active |= (1ull << A20_EVENT_READABLE) | (1ull << A20_EVENT_MESSAGE_READY);
        if (rev & POLLOUT)
            active |= 1ull << A20_EVENT_WRITABLE;
        if (rev & POLLERR)
            active |= 1ull << A20_EVENT_ERROR;
        if (rev & POLLHUP)
            active |= 1ull << A20_EVENT_CLOSED;
        if (rev & POLLNVAL) {
            a20_object_release(entry.object, entry.type);
            return -A20_ERR_BAD_HANDLE;
        }
        break;
    }
    case A20_OBJ_CHANNEL_ENDPOINT: {
        a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;
        if (ep->msg_count > 0)
            active |= (1ull << A20_EVENT_READABLE) | (1ull << A20_EVENT_MESSAGE_READY);
        if (ep->peer_closed)
            active |= 1ull << A20_EVENT_CLOSED;
        else if (ep->msg_count < ep->msg_cap)
            active |= 1ull << A20_EVENT_WRITABLE;
        break;
    }
    case A20_OBJ_TASK:
    case A20_OBJ_THREAD: {
        task_t *target = proc_find_get((int)(uintptr_t)entry.object);
        if (target) {
            if (target->state == PROC_ZOMBIE)
                active |= 1ull << A20_EVENT_EXITED;
            proc_put(target);
        } else {
            active |= 1ull << A20_EVENT_EXITED;
        }
        break;
    }
    default:
        /* timer/event-queue/etc: no synchronous level query yet */
        break;
    }

    a20_object_release(entry.object, entry.type);

    kargs.out_events = active & kargs.event_mask;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
