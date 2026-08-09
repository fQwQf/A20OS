#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "fs/io_uring.h"

/*
 * io_uring syscalls.  The ring is created in kernel/fs/io_uring.c; the setup
 * params struct mirrors the Linux io_uring_params fields the kernel writes.
 */

struct io_uring_params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t flags;
        uint32_t dropped;
        uint32_t array;
        uint32_t resv1;
        uint64_t user_addr;
    } sq_off;
    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t overflow;
        uint32_t cqes;
        uint64_t resv[2];
        uint64_t user_addr;
    } cq_off;
};

int64_t sys_io_uring_setup(unsigned entries, void *params)
{
    if (!params)
        return -EFAULT;

    int fd = io_uring_create(entries);
    if (fd < 0)
        return fd;

    /* The kernel-allocated ring pages are not mapped at a user address that
     * userland knows, so report the standard field layout with the ring mask
     * so direct SQ/CQ access can be driven through io_uring_enter(). */
    struct io_uring_params up;
    memset(&up, 0, sizeof(up));
    up.sq_entries = entries;
    up.cq_entries = entries;
    up.sq_off.ring_mask = 0;
    up.sq_off.ring_entries = 1;
    up.sq_off.user_addr = 0;
    up.cq_off.ring_mask = 0;
    up.cq_off.ring_entries = 1;
    up.cq_off.user_addr = 0;

    if (copy_to_user(params, &up, sizeof(up)) < 0) {
        sys_close(fd);
        return -EFAULT;
    }
    return fd;
}

int64_t sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                           unsigned flags, const void *sig, size_t sigsz)
{
    (void)sig;
    (void)sigsz;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return -EBADF;
    return io_uring_enter((int)gfd, to_submit, min_complete, flags);
}

int64_t sys_io_uring_register(int fd, unsigned opcode, const void *arg,
                              unsigned nr_args)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return -EBADF;
    return io_uring_register((int)gfd, opcode, arg, nr_args);
}
