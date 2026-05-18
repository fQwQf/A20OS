#include "a20_syscall.h"
#include "a20_types.h"

int64_t a20_handle_write_simple(uint32_t handle, const char *buf, uint64_t len)
{
    a20_iovec_t iov;
    iov.base = (uint64_t)buf;
    iov.len = len;

    a20_io_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.handle    = handle;
    args._pad0     = 0;
    args.iov       = (uint64_t)&iov;
    args.iov_count = 1;
    args._pad1     = 0;
    args.offset    = A20_OFFSET_CURRENT;
    args.out_count = 0;

    return a20_syscall6(A20_SYS_handle_write, (uint64_t)&args, 0, 0, 0, 0, 0);
}
