#ifndef _A20_HANDLE_H
#define _A20_HANDLE_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline a20_status_t a20_hdl_close(a20_handle_t h)
{
    return a20_syscall6(A20_SYS_handle_close, h, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_hdl_close_many(const a20_handle_t *handles, uint32_t count)
{
    return a20_syscall6(A20_SYS_handle_close_many,
                        (uint64_t)handles, count, 0, 0, 0, 0);
}

static inline a20_status_t a20_hdl_dup(const a20_handle_dup_args_t *args)
{
    return a20_syscall6(A20_SYS_handle_dup, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_hdl_transfer(const a20_handle_transfer_args_t *args)
{
    return a20_syscall6(A20_SYS_handle_transfer, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_hdl_query(a20_handle_t h, a20_handle_info_t *out)
{
    return a20_syscall6(A20_SYS_handle_query, h, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_hdl_replace(a20_handle_t h, a20_rights_t new_rights,
                                            a20_handle_t *out)
{
    return a20_syscall6(A20_SYS_handle_replace, h, new_rights, (uint64_t)out, 0, 0, 0);
}

static inline a20_status_t a20_hdl_seek(a20_handle_t h, int64_t offset, uint32_t whence)
{
    return a20_syscall6(A20_SYS_handle_seek, h,
                        (uint64_t)offset, (uint64_t)whence, 0, 0, 0);
}

static inline a20_status_t a20_hdl_read(a20_handle_t h, a20_iovec_t *iov,
                                         uint32_t iov_count, uint64_t *out_actual)
{
    a20_io_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.handle    = h;
    args._pad0     = 0;
    args.iov       = (uint64_t)iov;
    args.iov_count = iov_count;
    args._pad1     = 0;
    args.offset    = A20_OFFSET_CURRENT;
    args.out_count = 0;
    a20_status_t r = a20_syscall6(A20_SYS_handle_read, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (out_actual) *out_actual = args.out_count;
    return r;
}

static inline a20_status_t a20_hdl_write(a20_handle_t h, const a20_iovec_t *iov,
                                          uint32_t iov_count, uint64_t *out_actual)
{
    a20_io_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.handle    = h;
    args._pad0     = 0;
    args.iov       = (uint64_t)iov;
    args.iov_count = iov_count;
    args._pad1     = 0;
    args.offset    = A20_OFFSET_CURRENT;
    args.out_count = 0;
    a20_status_t r = a20_syscall6(A20_SYS_handle_write, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (out_actual) *out_actual = args.out_count;
    return r;
}

static inline a20_status_t a20_hdl_read_buf(a20_handle_t h, void *buf,
                                             uint64_t len, uint64_t *out_actual)
{
    a20_iovec_t iov = { (uint64_t)buf, len };
    return a20_hdl_read(h, &iov, 1, out_actual);
}

static inline a20_status_t a20_hdl_write_buf(a20_handle_t h, const void *buf,
                                              uint64_t len, uint64_t *out_actual)
{
    a20_iovec_t iov = { (uint64_t)buf, len };
    return a20_hdl_write(h, &iov, 1, out_actual);
}

static inline a20_status_t a20_hdl_stat(a20_handle_t h, a20_stat_t *out)
{
    return a20_syscall6(A20_SYS_handle_stat, h, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_hdl_xattr_set(a20_handle_t h, const char *key,
                                              const void *val, uint64_t val_len)
{
    return a20_syscall6(A20_SYS_handle_xattr_set, h,
                        (uint64_t)key, (uint64_t)val, val_len, 0, 0);
}

static inline a20_status_t a20_hdl_xattr_get(a20_handle_t h, const char *key,
                                             void *val, uint64_t val_len,
                                             uint64_t *out_len)
{
    a20_status_t r = a20_syscall6(A20_SYS_handle_xattr_get, h,
                                   (uint64_t)key, (uint64_t)val, val_len, (uint64_t)out_len, 0);
    return r;
}

#endif
