/*
 * A20OS liba20c — POSIX file I/O wrappers.
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "fdtable.h"
#include "../liba20rt/a20_syscall.h"

int open(const char *path, int flags, ...)
{
    uint32_t rights = 0;
    if (flags & O_RDONLY)  rights |= 1;
    if (flags & O_WRONLY)  rights |= 2;
    if (flags & O_RDWR)    rights |= 3;
    if (flags & O_CREAT)   rights |= 4;

    a20_path_open_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.flags      = 0;
    args.rights     = rights;
    args.path       = (uint64_t)(uintptr_t)path;
    args.path_len   = (uint32_t)strlen(path);
    args.mode       = 0644;
    args.out_handle = A20_HANDLE_NULL;

    int64_t h = a20_syscall6(A20_SYS_path_open, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (h < 0) return -1;
    return __fd_alloc(args.out_handle);
}

int close(int fd)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;
    __fd_free(fd);
    a20_syscall6(A20_SYS_handle_close, h, 0, 0, 0, 0, 0);
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;

    a20_iovec_t iov;
    iov.base = (uint64_t)(uintptr_t)buf;
    iov.len  = count;

    a20_io_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.handle     = h;
    args._pad0      = 0;
    args.iov        = (uint64_t)&iov;
    args.iov_count  = 1;
    args._pad1      = 0;
    args.offset     = A20_OFFSET_CURRENT;
    args.out_count  = 0;

    int64_t r = a20_syscall6(A20_SYS_handle_read, (uint64_t)&args, 0, 0, 0, 0, 0);
    return r < 0 ? -1 : (ssize_t)args.out_count;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;

    a20_iovec_t iov;
    iov.base = (uint64_t)(uintptr_t)buf;
    iov.len  = count;

    a20_io_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.handle     = h;
    args._pad0      = 0;
    args.iov        = (uint64_t)&iov;
    args.iov_count  = 1;
    args._pad1      = 0;
    args.offset     = A20_OFFSET_CURRENT;
    args.out_count  = 0;

    int64_t r = a20_syscall6(A20_SYS_handle_write, (uint64_t)&args, 0, 0, 0, 0, 0);
    return r < 0 ? -1 : (ssize_t)args.out_count;
}

off_t lseek(int fd, off_t offset, int whence)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;
    a20_off_t off = (a20_off_t)offset;
    int64_t r = a20_syscall6(A20_SYS_handle_seek, h,
                             (uint64_t)&off, (uint64_t)whence, 0, 0, 0);
    return r < 0 ? -1 : (off_t)off;
}
