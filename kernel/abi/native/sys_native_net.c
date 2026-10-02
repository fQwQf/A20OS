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
#include "net/socket_internal.h"
#include "fs/fdtable.h"
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

/* The handle's object is the socket vfile itself; the sock-level net_*
 * variants take the socket directly, so no fd is involved. */
static net_socket_t *native_sock_of(const a20_handle_entry_t *entry)
{
    return net_socket_from_vfile((vfile_t *)entry->object);
}

static int64_t a20_native_net_result(int64_t r)
{
    if (r >= 0) return r;
    switch (-r) {
    case EPERM:        return -A20_ERR_PERM;
    case EACCES:       return -A20_ERR_ACCESS;
    case EBADF:        return -A20_ERR_BAD_HANDLE;
    case EFAULT:       return -A20_ERR_FAULT;
    case ENOMEM:       return -A20_ERR_NO_MEMORY;
    case EINVAL:       return -A20_ERR_INVALID_ARGUMENT;
    case EAGAIN:       return -A20_ERR_WOULD_BLOCK;
    case ETIMEDOUT:    return -A20_ERR_TIMED_OUT;
    case ENOSPC:       return -A20_ERR_NO_SPACE;
    case EADDRINUSE:   return -A20_ERR_BUSY;
    case ENOTSOCK:     return -A20_ERR_TYPE_MISMATCH;
    case ENOPROTOOPT:  return -A20_ERR_NOT_SUPPORTED;
    case EOPNOTSUPP:   return -A20_ERR_NOT_SUPPORTED;
    default:           return -A20_ERR_IO;
    }
}

/* ===== Network (0x0600) ===== */

int64_t sys_a20_net_socket(const a20_syscall_args_t *args)
{
    int domain = (int)A20_ARG(0);
    int type = (int)A20_ARG(1);
    int protocol = (int)A20_ARG(2);

    int gfd = net_socket_create(domain, type, protocol);
    if (gfd < 0) return a20_native_net_result(gfd);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vfs_close(gfd); return -A20_ERR_BAD_HANDLE; }

    /* The handle owns its own vfile reference, independent of the fd. */
    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (!vf) { vfs_close(gfd); return -A20_ERR_BAD_HANDLE; }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    int64_t h = a20_handle_install(ht, vf, A20_OBJ_SOCKET, rights);
    if (h < 0) { vfs_put_file(vf); vfs_close(gfd); }
    return h;
}

/* The core net_* helpers take a sockaddr and a socklen_t* and dereference
 * both directly, so the native wrappers must stage them in kernel memory:
 * a raw user pointer would be read/written in supervisor mode. */
static int native_net_addr_in(uint8_t storage[NET_SOCKADDR_MAX],
                              const void *uaddr, size_t addrlen) {
    if (!uaddr || addrlen == 0) return -EINVAL;
    if (addrlen > NET_SOCKADDR_MAX) return -EINVAL;
    if (copy_from_user(storage, uaddr, addrlen) < 0) return -EFAULT;
    return 0;
}

static int native_net_addrlen_in(size_t *len, const void *uaddrlen) {
    uint32_t v;
    if (!uaddrlen) return -EFAULT;
    if (copy_from_user(&v, uaddrlen, sizeof(v)) < 0) return -EFAULT;
    if (v > NET_SOCKADDR_MAX) v = NET_SOCKADDR_MAX;
    *len = v;
    return 0;
}

static int native_net_addr_out(void *uaddr, const void *kaddr, size_t len) {
    if (!uaddr) return -EFAULT;
    if (copy_to_user(uaddr, kaddr, len) < 0) return -EFAULT;
    return 0;
}

static int native_net_addrlen_out(void *uaddrlen, size_t len) {
    uint32_t v = (uint32_t)len;
    if (!uaddrlen) return -EFAULT;
    return copy_to_user(uaddrlen, &v, sizeof(v)) < 0 ? -EFAULT : 0;
}

int64_t sys_a20_net_bind(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const void *addr = (const void *)(uintptr_t)A20_ARG(1);
    size_t addrlen = (size_t)A20_ARG(2);
    uint8_t kaddr[NET_SOCKADDR_MAX];
    int ar = native_net_addr_in(kaddr, addr, addrlen);
    if (ar < 0) return a20_native_net_result(ar);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    r = net_bind_sock(native_sock_of(&entry), kaddr, addrlen);
    a20_object_release(entry.object, entry.type);
    return a20_native_net_result(r);

}

int64_t sys_a20_net_connect(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const void *addr = (const void *)(uintptr_t)A20_ARG(1);
    size_t addrlen = (size_t)A20_ARG(2);
    uint8_t kaddr[NET_SOCKADDR_MAX];
    int ar = native_net_addr_in(kaddr, addr, addrlen);
    if (ar < 0) return a20_native_net_result(ar);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    r = net_connect_sock(native_sock_of(&entry), kaddr, addrlen);
    a20_object_release(entry.object, entry.type);
    return a20_native_net_result(r);

}

int64_t sys_a20_net_accept(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    void *addr = (void *)(uintptr_t)A20_ARG(1);
    size_t *addrlen = (size_t *)(uintptr_t)A20_ARG(2);
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen = 0;
    if (addr || addrlen) {
        int ar = native_net_addrlen_in(&klen, addrlen);
        if (ar < 0) return a20_native_net_result(ar);
    }

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    int new_gfd = net_accept_sock(native_sock_of(&entry),
                                  (addr && addrlen) ? kaddr : NULL,
                                  (addr && addrlen) ? &klen : NULL, 0);
    a20_object_release(entry.object, entry.type);
    if (new_gfd < 0) return a20_native_net_result(new_gfd);

    vfile_t *new_vf = fdtable_get_current_file_ref(new_gfd);

    if (addr && addrlen) {
        if (native_net_addr_out(addr, kaddr, klen) < 0 ||
            native_net_addrlen_out(addrlen, klen) < 0) {
            vfs_put_file(new_vf);
            fdtable_close_current(new_gfd);
            return a20_native_net_result(-EFAULT);
        }
    }


    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    if (!new_vf) {
        fdtable_close_current(new_gfd);
        return -A20_ERR_BAD_HANDLE;
    }
    int64_t nh = a20_handle_install(ht, new_vf, A20_OBJ_SOCKET, rights);
    if (nh < 0) {
        vfs_put_file(new_vf);
        fdtable_close_current(new_gfd);
    }
    return nh;
}

int64_t sys_a20_net_listen(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    int backlog = (int)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    r = net_listen_sock(native_sock_of(&entry), backlog);
    a20_object_release(entry.object, entry.type);
    return a20_native_net_result(r);

}

int64_t sys_a20_net_sendmsg(const a20_syscall_args_t *args)
{
    a20_net_sendmsg_args_t *uargs = (a20_net_sendmsg_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_net_sendmsg_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if (kargs.iov_count > 64) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.socket, A20_OBJ_SOCKET,
                                               A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t kaddrlen = 0;
    if (kargs.addr) {
        kaddrlen = sizeof(a20_net_addr_t);
        if (copy_from_user(kaddr, (const void *)(uintptr_t)kargs.addr, kaddrlen) < 0) {
            a20_object_release(entry.object, entry.type);
            return -A20_ERR_FAULT;
        }
    }

    net_socket_t *sock = native_sock_of(&entry);
    uint64_t total_sent = 0;
    char kbuf[512];

    a20_iovec_t *iov = (a20_iovec_t *)(uintptr_t)kargs.iov;
    for (uint32_t i = 0; i < kargs.iov_count; i++) {
        a20_iovec_t v;
        if (copy_from_user(&v, &iov[i], sizeof(v)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        uint64_t done = 0;
        while (done < v.len) {
            size_t chunk = v.len - done;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            if (copy_from_user(kbuf, (const void *)(uintptr_t)(v.base + done), chunk) < 0) {
                r = -A20_ERR_FAULT;
                goto out_entry;
            }
            int64_t n = net_sendto_sock(sock, kbuf, chunk, (int)kargs.flags,
                                   kaddrlen ? kaddr : NULL, kaddrlen);
            if (n < 0) {
                r = (total_sent > 0) ? (int64_t)total_sent : a20_native_net_result(n);
                goto out_entry;
            }
            done += (uint64_t)n;
            total_sent += (uint64_t)n;
            if ((size_t)n < chunk) break;
        }
    }

    kargs.out_sent = total_sent;
    a20_object_release(entry.object, entry.type);
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)total_sent;

out_entry:
    a20_object_release(entry.object, entry.type);
    return r;
}

int64_t sys_a20_net_recvmsg(const a20_syscall_args_t *args)
{
    a20_net_recvmsg_args_t *uargs = (a20_net_recvmsg_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_net_recvmsg_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if (kargs.iov_count > 64) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.socket, A20_OBJ_SOCKET,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    net_socket_t *sock = native_sock_of(&entry);
    uint64_t total_recv = 0;
    char kbuf[512];
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t kaddrlen = kargs.addr ? sizeof(kaddr) : 0;
    int have_addr = 0;

    a20_iovec_t *iov = (a20_iovec_t *)(uintptr_t)kargs.iov;
    for (uint32_t i = 0; i < kargs.iov_count; i++) {
        a20_iovec_t v;
        if (copy_from_user(&v, &iov[i], sizeof(v)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        uint64_t done = 0;
        while (done < v.len) {
            size_t chunk = v.len - done;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            int64_t n = net_recvfrom_socket_meta(sock, kbuf, chunk, (int)kargs.flags,
                                     kargs.addr ? kaddr : NULL,
                                     kargs.addr ? &kaddrlen : NULL,
                                     NULL);
            if (n < 0) {
                r = (total_recv > 0) ? (int64_t)total_recv : a20_native_net_result(n);
                goto out_entry;
            }
            if (n == 0) break;
            if (copy_to_user((void *)(uintptr_t)(v.base + done), kbuf, (size_t)n) < 0) {
                r = -A20_ERR_FAULT;
                goto out_entry;
            }
            have_addr = kargs.addr != 0;
            done += (uint64_t)n;
            total_recv += (uint64_t)n;
            if ((size_t)n < chunk) break;
        }
    }

    kargs.out_received = total_recv;
    kargs.out_addr_len = have_addr ? (uint32_t)kaddrlen : 0;
    a20_object_release(entry.object, entry.type);
    if (have_addr) {
        size_t out_len = kaddrlen < sizeof(a20_net_addr_t)
                         ? kaddrlen : sizeof(a20_net_addr_t);
        if (copy_to_user((void *)(uintptr_t)kargs.addr, kaddr, out_len) < 0)
            return -A20_ERR_FAULT;
    }
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)total_recv;

out_entry:
    a20_object_release(entry.object, entry.type);
    return r;
}

int64_t sys_a20_net_socketpair(const a20_syscall_args_t *args)
{
    int domain = (int)A20_ARG(0);
    int type = (int)A20_ARG(1);
    int protocol = (int)A20_ARG(2);
    a20_handle_t *out = (a20_handle_t *)(uintptr_t)A20_ARG(3);
    if (!out) return -A20_ERR_FAULT;

    int gfds[2];
    int r = net_socketpair_create(domain, type, protocol, gfds);
    if (r < 0) return a20_native_net_result(r);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vfs_close(gfds[0]); vfs_close(gfds[1]); return -A20_ERR_BAD_HANDLE; }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    vfile_t *vf0 = fdtable_get_current_file_ref(gfds[0]);
    vfile_t *vf1 = fdtable_get_current_file_ref(gfds[1]);
    if (!vf0 || !vf1) {
        if (vf0) vfs_put_file(vf0);
        if (vf1) vfs_put_file(vf1);
        vfs_close(gfds[0]);
        vfs_close(gfds[1]);
        return -A20_ERR_BAD_HANDLE;
    }
    int64_t h0 = a20_handle_install(ht, vf0, A20_OBJ_SOCKET, rights);
    int64_t h1 = a20_handle_install(ht, vf1, A20_OBJ_SOCKET, rights);
    if (h0 < 0 || h1 < 0) {
        /* a removed handle's release path drops its vfile reference via
         * a20_object_release; a never-installed reference is dropped here. */
        if (h0 >= 0) a20_handle_remove(ht, (a20_handle_t)h0);
        else vfs_put_file(vf0);
        if (h1 >= 0) a20_handle_remove(ht, (a20_handle_t)h1);
        else vfs_put_file(vf1);
        vfs_close(gfds[0]);
        vfs_close(gfds[1]);
        return (h0 < 0) ? h0 : h1;
    }

    a20_handle_t result[2];
    result[0] = (a20_handle_t)h0;
    result[1] = (a20_handle_t)h1;
    if (copy_to_user(out, result, sizeof(result)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h0);
        a20_handle_remove(ht, (a20_handle_t)h1);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_net_getname(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    void *addr = (void *)(uintptr_t)A20_ARG(1);
    size_t *addrlen = (size_t *)(uintptr_t)A20_ARG(2);
    int peer = (int)A20_ARG(3);
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen = 0;
    if (addr || addrlen) {
        int ar = native_net_addrlen_in(&klen, addrlen);
        if (ar < 0) return a20_native_net_result(ar);
    }

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    if (peer)
        r = net_getpeername_sock(native_sock_of(&entry),
                            (addr && addrlen) ? kaddr : NULL,
                            (addr && addrlen) ? &klen : NULL);
    else
        r = net_getsockname_sock(native_sock_of(&entry),
                            (addr && addrlen) ? kaddr : NULL,
                            (addr && addrlen) ? &klen : NULL);
    a20_object_release(entry.object, entry.type);
    if (r < 0) return a20_native_net_result(r);
    if (addr && addrlen) {
        if (native_net_addr_out(addr, kaddr, klen) < 0 ||
            native_net_addrlen_out(addrlen, klen) < 0)
            return a20_native_net_result(-EFAULT);
    }
    return a20_native_net_result(r);

}

int64_t sys_a20_net_shutdown(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    int how = (int)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    r = net_shutdown_sock(native_sock_of(&entry), how);
    a20_object_release(entry.object, entry.type);
    return a20_native_net_result(r);

}
