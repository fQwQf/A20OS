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

static int64_t native_socket_lookup(struct a20_ht_internal *ht, a20_handle_t handle,
                                     a20_rights_t rights, a20_handle_entry_t *entry)
{
    int64_t result = a20_handle_lookup_ref_internal(ht, handle, A20_OBJ_INVALID, 0, entry);
    if (result < 0) return result;
    if (entry->type != A20_OBJ_SOCKET) result = -A20_ERR_NOT_SOCKET;
    else if ((entry->rights & rights) != rights) result = -A20_ERR_ACCESS;
    if (result < 0) a20_object_release(entry->object, entry->type);
    return result;
}

_Static_assert(sizeof(a20_net_sendmsg_args_t) == 72 &&
    offsetof(a20_net_sendmsg_args_t, addr_length) == 64, "Native sendmsg v1 prefix");
_Static_assert(sizeof(a20_net_recvmsg_args_t) == 80 &&
    offsetof(a20_net_recvmsg_args_t, addr_capacity) == 72, "Native recvmsg v1 prefix");

int64_t a20_native_net_result(int64_t r)
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
    case EADDRINUSE:   return -A20_ERR_ADDRESS_IN_USE;
    case ENOTSOCK:     return -A20_ERR_NOT_SOCKET;
    case ENOPROTOOPT:  return -A20_ERR_PROTOCOL_OPTION;
    case EOPNOTSUPP:   return -A20_ERR_NOT_SUPPORTED;
    case EPIPE: return -A20_ERR_BROKEN_PIPE;
    case EADDRNOTAVAIL: return -A20_ERR_ADDRESS_NOT_AVAILABLE;
    case ECONNREFUSED: return -A20_ERR_CONNECTION_REFUSED;
    case ECONNRESET: return -A20_ERR_CONNECTION_RESET;
    case ENOTCONN: return -A20_ERR_NOT_CONNECTED;
    case EISCONN: return -A20_ERR_ALREADY_CONNECTED;
    case EINPROGRESS: return -A20_ERR_IN_PROGRESS;
    case EALREADY: return -A20_ERR_ALREADY_IN_PROGRESS;
    case ENETUNREACH: return -A20_ERR_NETWORK_UNREACHABLE;
    case EHOSTUNREACH: return -A20_ERR_HOST_UNREACHABLE;
    case EMSGSIZE: return -A20_ERR_MESSAGE_TOO_LARGE;
    case EINTR:
    case ERESTARTSYS: return -A20_ERR_INTERRUPTED;
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
    if (h < 0) vfs_put_file(vf);
    fdtable_close_current(gfd);
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
    int64_t r = native_socket_lookup(ht, h, A20_RIGHT_CONTROL, &entry);
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
    int64_t r = native_socket_lookup(ht, h, A20_RIGHT_WRITE, &entry);
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
    int64_t r = native_socket_lookup(ht, h, A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    int new_gfd = net_accept_sock(native_sock_of(&entry),
                                  (addr && addrlen) ? kaddr : NULL,
                                  (addr && addrlen) ? &klen : NULL, (int)A20_ARG(3));
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
    if (nh < 0) vfs_put_file(new_vf);
    fdtable_close_current(new_gfd);
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
    int64_t r = native_socket_lookup(ht, h, A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    r = net_listen_sock(native_sock_of(&entry), backlog);
    a20_object_release(entry.object, entry.type);
    return a20_native_net_result(r);

}

/* Version 1 uses the fixed IP address record. Version 2 appends explicit
 * sockaddr lengths and receive flags; both retain the original prefix. */
static int native_ip_to_sockaddr(const a20_net_addr_t *native,
                                  uint8_t storage[NET_SOCKADDR_MAX], size_t *length)
{
    memset(storage, 0, NET_SOCKADDR_MAX);
    if (native->_pad) return -A20_ERR_INVALID_ARGUMENT;
    if (native->family == AF_INET) {
        net_sockaddr_in_t value = {.sin_family = AF_INET, .sin_port = native->port};
        memcpy(&value.sin_addr, native->addr, 4);
        memcpy(storage, &value, sizeof(value));
        *length = sizeof(value);
    } else if (native->family == AF_INET6) {
        net_sockaddr_in6_t value = {.sin6_family = AF_INET6, .sin6_port = native->port};
        memcpy(value.sin6_addr, native->addr, 16);
        memcpy(storage, &value, sizeof(value));
        *length = sizeof(value);
    } else return -A20_ERR_NOT_SUPPORTED;
    return 0;
}

static int native_sockaddr_to_ip(const uint8_t *storage, size_t length, a20_net_addr_t *native)
{
    memset(native, 0, sizeof(*native));
    if (length < 2) return -A20_ERR_INVALID_ARGUMENT;
    memcpy(&native->family, storage, 2);
    if (native->family == AF_INET && length >= sizeof(net_sockaddr_in_t)) {
        const net_sockaddr_in_t *value = (const void *)storage;
        native->port = value->sin_port;
        memcpy(native->addr, &value->sin_addr, 4);
    } else if (native->family == AF_INET6 && length >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *value = (const void *)storage;
        native->port = value->sin6_port;
        memcpy(native->addr, value->sin6_addr, 16);
    } else return -A20_ERR_NOT_SUPPORTED;
    return 0;
}

static int endpoint_to_sockaddr(const a20_net_endpoint_t *source,
                                uint8_t storage[NET_SOCKADDR_MAX], size_t *length)
{
    if (source->size < sizeof(*source) || source->version != 1 || source->reserved)
        return -A20_ERR_INVALID_ARGUMENT;
    memset(storage, 0, NET_SOCKADDR_MAX);
    uint16_t port = (source->port >> 8) | (source->port << 8);
    if (source->family == A20_SOCKET_FAMILY_LOCAL) {
        if (source->length > sizeof(source->local_name) || source->port || source->scope)
            return -A20_ERR_INVALID_ARGUMENT;
        uint16_t family = AF_UNIX; memcpy(storage, &family, 2);
        memcpy(storage + 2, source->local_name, source->length);
        *length = 2 + source->length;
    } else if (source->family == A20_SOCKET_FAMILY_IPV4 && source->length == 4 && !source->scope) {
        net_sockaddr_in_t value = {.sin_family = AF_INET, .sin_port = port};
        memcpy(&value.sin_addr, source->address, 4); memcpy(storage, &value, sizeof(value));
        *length = sizeof(value);
    } else if (source->family == A20_SOCKET_FAMILY_IPV6 && source->length == 16) {
        net_sockaddr_in6_t value = {.sin6_family = AF_INET6, .sin6_port = port,
                                   .sin6_scope_id = source->scope};
        memcpy(value.sin6_addr, source->address, 16); memcpy(storage, &value, sizeof(value));
        *length = sizeof(value);
    } else return -A20_ERR_INVALID_ARGUMENT;
    return 0;
}
static int sockaddr_to_endpoint(const uint8_t *storage, size_t length, a20_net_endpoint_t *destination)
{
    memset(destination, 0, sizeof(*destination));
    destination->size = sizeof(*destination); destination->version = 1;
    uint16_t family = 0; if (length < 2) return -A20_ERR_INVALID_ARGUMENT;
    memcpy(&family, storage, 2);
    if (family == AF_UNIX && length <= 110) {
        destination->family = A20_SOCKET_FAMILY_LOCAL; destination->length = length - 2;
        memcpy(destination->local_name, storage + 2, length - 2);
    } else if (family == AF_INET && length >= sizeof(net_sockaddr_in_t)) {
        const net_sockaddr_in_t *value = (const void *)storage;
        destination->family = A20_SOCKET_FAMILY_IPV4; destination->length = 4;
        destination->port = (value->sin_port >> 8) | (value->sin_port << 8);
        memcpy(destination->address, &value->sin_addr, 4);
    } else if (family == AF_INET6 && length >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *value = (const void *)storage;
        destination->family = A20_SOCKET_FAMILY_IPV6; destination->length = 16;
        destination->port = (value->sin6_port >> 8) | (value->sin6_port << 8);
        destination->scope = value->sin6_scope_id;
        memcpy(destination->address, value->sin6_addr, 16);
    } else return -A20_ERR_NOT_SUPPORTED;
    return 0;
}

static int64_t native_net_message(const a20_syscall_args_t *args, int receiving)
{
    void *user = (void *)(uintptr_t)A20_ARG(0);
    uint32_t header[2];
    if (!user || copy_from_user(header, user, sizeof(header)) < 0) return -A20_ERR_FAULT;
    uint32_t minimum = receiving ? 72u : 64u;
    if (header[1] == 2) minimum = receiving ? sizeof(a20_net_recvmsg_args_t) : sizeof(a20_net_sendmsg_args_t);
    if (header[1] < 1 || header[1] > 2 || header[0] < minimum) return -A20_ERR_INVALID_ARGUMENT;
    a20_net_sendmsg_args_t send = {0};
    a20_net_recvmsg_args_t recv = {0};
    a20_handle_t handle;
    uint64_t iov_address, address;
    uint32_t count, flags, address_length, control_length;
    size_t known = receiving ? sizeof(recv) : sizeof(send);
    size_t bytes = header[0] < known ? header[0] : known;
    if (copy_from_user(receiving ? (void *)&recv : (void *)&send, user, bytes) < 0) return -A20_ERR_FAULT;
    if (receiving) {
        if (header[1] == 2 && recv._v1_tail_padding) return -A20_ERR_INVALID_ARGUMENT;
        handle = recv.socket; iov_address = recv.iov; count = recv.iov_count;
        flags = recv.flags; address = recv.addr; address_length = recv.addr_capacity;
        control_length = recv.control_len;
    } else {
        handle = send.socket; iov_address = send.iov; count = send.iov_count;
        flags = send.flags; address = send.addr; address_length = send.addr_length;
        control_length = send.control_len;
        if (header[1] == 2 && send.reserved) return -A20_ERR_INVALID_ARGUMENT;
    }
    if (count > 1024) return -A20_ERR_INVALID_ARGUMENT;
    if (header[1] == 2) {
        uint32_t allowed = receiving ? 15u : A20_NET_NONBLOCK;
        if (flags & ~allowed) return -A20_ERR_INVALID_ARGUMENT;
        uint32_t native_flags = flags; flags = 0;
        if (native_flags & A20_NET_NONBLOCK) flags |= MSG_DONTWAIT;
        if (native_flags & A20_NET_PEEK) flags |= MSG_PEEK;
        if (native_flags & A20_NET_WAIT_ALL) flags |= MSG_WAITALL;
        if (native_flags & A20_NET_FULL_LENGTH) flags |= MSG_TRUNC;
        if (address && address_length < sizeof(a20_net_endpoint_t)) return -A20_ERR_INVALID_ARGUMENT;
    }
    /* Native handle transfer needs its own control schema. Never silently
     * report success for a POSIX descriptor-valued send request. */
    if (control_length && (!receiving || header[1] == 1)) return -A20_ERR_NOT_SUPPORTED;
    struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
    if (!ht) return -A20_ERR_BAD_HANDLE;
    a20_handle_entry_t entry;
    int64_t result = native_socket_lookup(ht, handle, receiving ? A20_RIGHT_READ : A20_RIGHT_WRITE, &entry);
    if (result < 0) return result;
    a20_iovec_t local_iov[8], *iov = local_iov;
    char *buffer = NULL;
    net_recv_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (receiving ? a20_ht_get_label(ht) < entry.security_label :
                    a20_ht_get_label(ht) > entry.security_label) {
        result = -A20_ERR_ACCESS; goto out;
    }
    if (count > 8) {
        iov = kmalloc(count * sizeof(*iov));
        if (!iov) { result = -A20_ERR_NO_MEMORY; goto out; }
    }
    if (count && copy_from_user(iov, (void *)(uintptr_t)iov_address, count * sizeof(*iov)) < 0) {
        result = -A20_ERR_FAULT; goto out;
    }
    uint64_t requested = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (iov[i].len > INT64_MAX - requested) { result = -A20_ERR_INVALID_ARGUMENT; goto out; }
        requested += iov[i].len;
    }
    net_socket_t *socket = native_sock_of(&entry);
    int datagram = socket->type != SOCK_STREAM;
    if (!receiving && datagram && requested > 1048576) { result = -A20_ERR_MESSAGE_TOO_LARGE; goto out; }
    size_t capacity = requested;
    size_t limit = datagram ? 1048576u : 65536u;
    if (capacity > limit) capacity = limit;
    buffer = kmalloc(capacity ? capacity : 1);
    if (!buffer) { result = -A20_ERR_NO_MEMORY; goto out; }
    uint8_t storage[NET_SOCKADDR_MAX] = {0};
    size_t sockaddr_length = 0;
    if (!receiving && address) {
        if (header[1] == 2) {
            a20_net_endpoint_t endpoint;
            if (copy_from_user(&endpoint, (void *)(uintptr_t)address, sizeof(endpoint)) < 0) { result = -A20_ERR_FAULT; goto out; }
            result = endpoint_to_sockaddr(&endpoint, storage, &sockaddr_length);
            if (result < 0) goto out;
        } else {
            a20_net_addr_t native;
            if (copy_from_user(&native, (void *)(uintptr_t)address, sizeof(native)) < 0) { result = -A20_ERR_FAULT; goto out; }
            result = native_ip_to_sockaddr(&native, storage, &sockaddr_length);
            if (result < 0) goto out;
        }
    }
    size_t transferred = 0;
    if (!receiving) {
        for (uint32_t i = 0; i < count && transferred < capacity; i++) {
            size_t size = iov[i].len < capacity - transferred ? iov[i].len : capacity - transferred;
            if (size && copy_from_user(buffer + transferred, (void *)(uintptr_t)iov[i].base, size) < 0) { result = -A20_ERR_FAULT; goto out; }
            transferred += size;
        }
        /* One call preserves datagram boundaries and stream short-write rules. */
        result = a20_native_net_result(net_sendto_sock(socket, buffer, capacity, flags,
            address ? storage : NULL, sockaddr_length));
        if (result < 0) goto out;
        send.out_sent = (uint64_t)result;
        result = a20_copy_struct_to_user(user, &send, sizeof(send)) < 0 ? -A20_ERR_FAULT : result;
    } else {
        sockaddr_length = sizeof(storage);
        /* Learn the full datagram size even when the caller only wants a
         * prefix. MSG_TRUNC input controls the returned count, not copying. */
        int read_flags = datagram ? flags | MSG_TRUNC : flags;
        result = a20_native_net_result(net_recvfrom_socket_meta(socket, buffer, capacity, read_flags,
            address ? storage : NULL, address ? &sockaddr_length : NULL, &meta));
        if (result < 0) goto out;
        uint64_t actual = result;
        size_t copied_limit = actual < capacity ? (size_t)actual : capacity;
        for (uint32_t i = 0; i < count && transferred < copied_limit; i++) {
            size_t size = iov[i].len < copied_limit - transferred ? iov[i].len : copied_limit - transferred;
            if (size && copy_to_user((void *)(uintptr_t)iov[i].base, buffer + transferred, size) < 0) {
                if (!transferred) { result = -A20_ERR_FAULT; goto out; }
                copied_limit = transferred;
                actual = transferred;
                break;
            }
            transferred += size;
        }
        recv.out_received = datagram && (flags & MSG_TRUNC) ? actual : copied_limit;
        recv.out_flags = actual > capacity ? MSG_TRUNC : 0;
        uint64_t socket_flags = net_sock_lock(socket);
        int ancillary = meta.scm_nfiles || (meta.has_cred && socket->passcred) ||
            (meta.has_pktinfo && (socket->ipv6_recv_pktinfo || socket->ipv6_recv_2292_pktinfo)) ||
            (meta.has_hoplimit && (socket->ipv6_recv_hoplimit || socket->ipv6_recv_2292_hoplimit)) ||
            (meta.has_tclass && socket->ipv6_recv_tclass);
        net_sock_unlock(socket, socket_flags);
        if (ancillary) recv.out_flags |= MSG_CTRUNC;
        if (header[1] == 2) recv.out_flags =
            ((recv.out_flags & MSG_TRUNC) ? A20_NET_DATA_TRUNCATED : 0) |
            ((recv.out_flags & MSG_CTRUNC) ? A20_NET_METADATA_TRUNCATED : 0);
        recv.control_len = 0;
        recv.out_addr_len = 0;
        if (address) {
            if (sockaddr_length > sizeof(storage)) { result = -A20_ERR_IO; goto out; }
            if (header[1] == 2) {
                a20_net_endpoint_t endpoint;
                result = sockaddr_to_endpoint(storage, sockaddr_length, &endpoint);
                if (result < 0) goto out;
                if (copy_to_user((void *)(uintptr_t)address, &endpoint, sizeof(endpoint)) < 0) { result = -A20_ERR_FAULT; goto out; }
                recv.out_addr_len = sizeof(endpoint);
            } else {
                a20_net_addr_t native;
                result = native_sockaddr_to_ip(storage, sockaddr_length, &native);
                if (result < 0) goto out;
                if (copy_to_user((void *)(uintptr_t)address, &native, sizeof(native)) < 0) { result = -A20_ERR_FAULT; goto out; }
                recv.out_addr_len = sizeof(native);
            }
        }
        result = recv.out_received;
        if (a20_copy_struct_to_user(user, &recv, sizeof(recv)) < 0) result = -A20_ERR_FAULT;
    }
out:
    net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
    kfree(buffer);
    if (iov != local_iov) kfree(iov);
    a20_object_release(entry.object, entry.type);
    return result;
}

int64_t sys_a20_net_sendmsg(const a20_syscall_args_t *args)
{
    return native_net_message(args, 0);
}
int64_t sys_a20_net_recvmsg(const a20_syscall_args_t *args)
{
    return native_net_message(args, 1);
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

    fdtable_close_current(gfds[0]);
    fdtable_close_current(gfds[1]);
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
    int64_t r = native_socket_lookup(ht, h, A20_RIGHT_STAT, &entry);
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
    int64_t r = native_socket_lookup(ht, h, A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    r = net_shutdown_sock(native_sock_of(&entry), how);
    a20_object_release(entry.object, entry.type);
    return a20_native_net_result(r);

}
