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

/* ===== Network (0x0600) ===== */

int64_t sys_a20_net_socket(const a20_syscall_args_t *args)
{
    int domain = (int)A20_ARG(0);
    int type = (int)A20_ARG(1);
    int protocol = (int)A20_ARG(2);

    int gfd = net_socket_create(domain, type, protocol);
    if (gfd < 0) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vfs_close(gfd); return -A20_ERR_BAD_HANDLE; }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)gfd, A20_OBJ_SOCKET, rights);
    if (h < 0) vfs_close(gfd);
    return h;
}

int64_t sys_a20_net_bind(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const void *addr = (const void *)A20_ARG(1);
    size_t addrlen = (size_t)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    r = net_bind((int)(uintptr_t)entry.object, addr, addrlen);
    a20_object_release(entry.object, entry.type);
    return r;

}

int64_t sys_a20_net_connect(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const void *addr = (const void *)A20_ARG(1);
    size_t addrlen = (size_t)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    r = net_connect((int)(uintptr_t)entry.object, addr, addrlen);
    a20_object_release(entry.object, entry.type);
    return r;

}

int64_t sys_a20_net_accept(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    void *addr = (void *)A20_ARG(1);
    size_t *addrlen = (size_t *)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    int new_gfd = net_accept((int)(uintptr_t)entry.object, addr, addrlen, 0);
    a20_object_release(entry.object, entry.type);
    if (new_gfd < 0) return -A20_ERR_IO;


    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    int64_t nh = a20_handle_install(ht, (void *)(uintptr_t)new_gfd, A20_OBJ_SOCKET, rights);
    if (nh < 0) vfs_close(new_gfd);
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

    r = net_listen((int)(uintptr_t)entry.object, backlog);
    a20_object_release(entry.object, entry.type);
    return r;

}

int64_t sys_a20_net_sendmsg(const a20_syscall_args_t *args)
{
    a20_net_sendmsg_args_t *uargs = (a20_net_sendmsg_args_t *)A20_ARG(0);
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
        if (copy_from_user(kaddr, (const void *)kargs.addr, kaddrlen) < 0) {
            a20_object_release(entry.object, entry.type);
            return -A20_ERR_FAULT;
        }
    }

    int gfd = (int)(uintptr_t)entry.object;
    uint64_t total_sent = 0;
    char kbuf[512];

    a20_iovec_t *iov = (a20_iovec_t *)kargs.iov;
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
            if (copy_from_user(kbuf, (const void *)(v.base + done), chunk) < 0) {
                r = -A20_ERR_FAULT;
                goto out_entry;
            }
            int64_t n = net_sendto(gfd, kbuf, chunk, (int)kargs.flags,
                                   kaddrlen ? kaddr : NULL, kaddrlen);
            if (n < 0) {
                r = (total_sent > 0) ? (int64_t)total_sent : -A20_ERR_IO;
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
    a20_net_recvmsg_args_t *uargs = (a20_net_recvmsg_args_t *)A20_ARG(0);
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

    int gfd = (int)(uintptr_t)entry.object;
    uint64_t total_recv = 0;
    char kbuf[512];
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t kaddrlen = kargs.addr ? sizeof(kaddr) : 0;
    int have_addr = 0;

    a20_iovec_t *iov = (a20_iovec_t *)kargs.iov;
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
            int64_t n = net_recvfrom(gfd, kbuf, chunk, (int)kargs.flags,
                                     kargs.addr ? kaddr : NULL,
                                     kargs.addr ? &kaddrlen : NULL);
            if (n < 0) {
                r = (total_recv > 0) ? (int64_t)total_recv : -A20_ERR_IO;
                goto out_entry;
            }
            if (n == 0) break;
            if (copy_to_user((void *)(v.base + done), kbuf, (size_t)n) < 0) {
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
        if (copy_to_user((void *)kargs.addr, kaddr, out_len) < 0)
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
    a20_handle_t *out = (a20_handle_t *)A20_ARG(3);
    if (!out) return -A20_ERR_FAULT;

    int gfds[2];
    int r = net_socketpair_create(domain, type, protocol, gfds);
    if (r < 0) return -A20_ERR_IO;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vfs_close(gfds[0]); vfs_close(gfds[1]); return -A20_ERR_BAD_HANDLE; }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER;
    int64_t h0 = a20_handle_install(ht, (void *)(uintptr_t)gfds[0], A20_OBJ_SOCKET, rights);
    int64_t h1 = a20_handle_install(ht, (void *)(uintptr_t)gfds[1], A20_OBJ_SOCKET, rights);
    if (h0 < 0 || h1 < 0) {
        if (h0 >= 0) a20_handle_remove(ht, (a20_handle_t)h0);
        else vfs_close(gfds[0]);
        if (h1 >= 0) a20_handle_remove(ht, (a20_handle_t)h1);
        else vfs_close(gfds[1]);
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
    void *addr = (void *)A20_ARG(1);
    size_t *addrlen = (size_t *)A20_ARG(2);
    int peer = (int)A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_SOCKET,
                                               A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    if (peer)
        r = net_getpeername((int)(uintptr_t)entry.object, addr, addrlen);
    else
        r = net_getsockname((int)(uintptr_t)entry.object, addr, addrlen);
    a20_object_release(entry.object, entry.type);
    return r;

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

    r = net_shutdown((int)(uintptr_t)entry.object, how);
    a20_object_release(entry.object, entry.type);
    return r;

}
