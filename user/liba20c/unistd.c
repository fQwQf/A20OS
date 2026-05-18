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

    uint64_t args[6] = {0, (uint64_t)(uintptr_t)path, (uint64_t)strlen(path),
                        rights, 0, 0};
    int64_t h = a20_path_open(args);
    if ((int64_t)h < 0) return -1;
    return __fd_alloc((uint32_t)h);
}

int close(int fd)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;
    __fd_free(fd);
    a20_handle_close(h);
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;
    uint64_t args[4] = {h, (uint64_t)(uintptr_t)buf, count, 0};
    int64_t r = a20_handle_read(args);
    return r < 0 ? -1 : (ssize_t)r;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;
    uint64_t args[4] = {h, (uint64_t)(uintptr_t)buf, count, 0};
    int64_t r = a20_handle_write(args);
    return r < 0 ? -1 : (ssize_t)r;
}

off_t lseek(int fd, off_t offset, int whence)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) return -1;
    int64_t r = a20_handle_seek(h, (uint64_t)offset, (uint64_t)whence);
    return r < 0 ? -1 : (off_t)r;
}
