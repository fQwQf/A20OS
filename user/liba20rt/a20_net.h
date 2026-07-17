/*
 * A20OS Native SDK — Network syscall wrappers (0x0600).
 */
#ifndef _A20_NET_H
#define _A20_NET_H

#include "a20_types.h"
#include "a20_syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline a20_status_t a20_net_socket(int domain, int type, int protocol,
                                            a20_rights_t rights,
                                            a20_handle_t *out_socket)
{
    (void)rights;

    a20_status_t r = a20_syscall6(A20_SYS_net_socket,
                                    (uint64_t)(uint32_t)domain,
                                    (uint64_t)(uint32_t)type,
                                    (uint64_t)(uint32_t)protocol,
                                    0, 0, 0);
    if (out_socket)
        *out_socket = (r >= 0) ? (a20_handle_t)r : A20_HANDLE_NULL;
    return r;
}

static inline a20_status_t a20_net_bind(a20_handle_t socket,
                                        const a20_net_addr_t *addr,
                                        uint64_t addr_len)
{
    return a20_syscall6(A20_SYS_net_bind, socket,
                        (uint64_t)addr, addr_len, 0, 0, 0);
}

static inline a20_status_t a20_net_connect(a20_handle_t socket,
                                           const a20_net_addr_t *addr,
                                           uint64_t addr_len)
{
    return a20_syscall6(A20_SYS_net_connect, socket,
                        (uint64_t)addr, addr_len, 0, 0, 0);
}

static inline a20_status_t a20_net_listen(a20_handle_t socket, int backlog)
{
    return a20_syscall6(A20_SYS_net_listen, socket,
                        (uint64_t)(uint32_t)backlog, 0, 0, 0, 0);
}

static inline a20_status_t a20_net_accept(a20_handle_t socket,
                                          a20_rights_t rights,
                                          a20_handle_t *out_socket,
                                          a20_net_addr_t *out_addr,
                                          uint64_t *out_addr_len)
{
    (void)rights;

    a20_status_t r = a20_syscall6(A20_SYS_net_accept,
                                    socket, (uint64_t)out_addr,
                                    (uint64_t)out_addr_len, 0, 0, 0);
    if (out_socket)
        *out_socket = (r >= 0) ? (a20_handle_t)r : A20_HANDLE_NULL;
    return r;
}

static inline a20_status_t a20_net_sendmsg(a20_handle_t socket,
                                           const a20_iovec_t *iov,
                                           uint32_t iov_count,
                                           uint32_t flags,
                                           const a20_net_addr_t *addr,
                                           const void *control,
                                           uint32_t control_len,
                                           uint64_t *out_sent)
{
    a20_net_sendmsg_args_t args;
    args.size         = sizeof(args);
    args.version      = 1;
    args.socket       = socket;
    args.iov          = (uint64_t)iov;
    args.iov_count    = iov_count;
    args.flags        = flags;
    args.addr         = (uint64_t)addr;
    args.control      = (uint64_t)control;
    args.control_len  = control_len;
    args.out_sent     = 0;

    a20_status_t r = a20_syscall6(A20_SYS_net_sendmsg,
                                  (uint64_t)&args, 0, 0, 0, 0, 0);
    if (out_sent)
        *out_sent = args.out_sent;
    return r;
}

static inline a20_status_t a20_net_recvmsg(a20_handle_t socket,
                                           a20_iovec_t *iov,
                                           uint32_t iov_count,
                                           uint32_t flags,
                                           a20_net_addr_t *addr,
                                           void *control,
                                           uint32_t control_len,
                                           uint64_t *out_received,
                                           uint32_t *out_addr_len)
{
    a20_net_recvmsg_args_t args;
    args.size          = sizeof(args);
    args.version       = 1;
    args.socket        = socket;
    args.iov           = (uint64_t)iov;
    args.iov_count     = iov_count;
    args.flags         = flags;
    args.addr          = (uint64_t)addr;
    args.control       = (uint64_t)control;
    args.control_len   = control_len;
    args.out_received  = 0;
    args.out_addr_len  = out_addr_len ? *out_addr_len : 0;

    a20_status_t r = a20_syscall6(A20_SYS_net_recvmsg,
                                  (uint64_t)&args, 0, 0, 0, 0, 0);
    if (out_received)
        *out_received = args.out_received;
    if (out_addr_len)
        *out_addr_len = args.out_addr_len;
    return r;
}

static inline a20_status_t a20_net_socketpair(int domain, int type, int protocol,
                                              a20_handle_t out_sockets[2])
{
    return a20_syscall6(A20_SYS_net_socketpair,
                        (uint64_t)(uint32_t)domain,
                        (uint64_t)(uint32_t)type,
                        (uint64_t)(uint32_t)protocol,
                        (uint64_t)out_sockets, 0, 0);
}

static inline a20_status_t a20_net_getname(a20_handle_t socket,
                                           a20_net_addr_t *out_addr,
                                           uint64_t *out_len,
                                           int peer)
{
    return a20_syscall6(A20_SYS_net_getname,
                        socket, (uint64_t)out_addr,
                        (uint64_t)out_len, (uint64_t)(uint32_t)peer,
                        0, 0);
}

static inline a20_status_t a20_net_shutdown(a20_handle_t socket, int how)
{
    return a20_syscall6(A20_SYS_net_shutdown, socket,
                        (uint64_t)(uint32_t)how, 0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif
