#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include "fdtable.h"
#include "../liba20rt/a20_handle.h"

int fcntl(int fd, int cmd, ...)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }

    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    switch (cmd) {
    case F_DUPFD: {
        int new_fd = __fd_alloc_from(0xFFFFFFFF, arg);
        if (new_fd < 0) {
            errno = EMFILE;
            return -1;
        }

        a20_handle_dup_args_t dup_args;
        dup_args.size        = sizeof(dup_args);
        dup_args.version     = 1;
        dup_args.source      = h;
        dup_args.flags       = 0;
        dup_args.rights_mask = A20_RIGHTS_ALL;
        dup_args.out_handle  = A20_HANDLE_NULL;
        dup_args.reserved    = 0;

        a20_status_t r = a20_hdl_dup(&dup_args);
        if (A20_IS_ERROR(r)) {
            __fd_free(new_fd);
            errno = EBADF;
            return -1;
        }

        __fd_set_handle(new_fd, dup_args.out_handle);
        __fd_set_open_flags(new_fd, __fd_get_open_flags(fd));
        __fd_set_fd_flags(new_fd, __fd_get_fd_flags(fd) & ~FD_CLOEXEC);
        return new_fd;
    }
    case F_GETFD:
        return (int)(__fd_get_fd_flags(fd) & FD_CLOEXEC);
    case F_SETFD:
        __fd_set_fd_flags(fd, (__fd_get_fd_flags(fd) & ~FD_CLOEXEC) |
                              ((uint32_t)arg & FD_CLOEXEC));
        return 0;
    case F_GETFL:
        return (int)__fd_get_open_flags(fd);
    case F_SETFL: {
        uint32_t mask = O_APPEND | O_NONBLOCK;
        __fd_set_open_flags(fd, (__fd_get_open_flags(fd) & ~mask) |
                                ((uint32_t)arg & mask));
        return 0;
    }
    default:
        errno = EINVAL;
        return -1;
    }
}
