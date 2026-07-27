#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "ipc/signalfd.h"

int64_t sys_signalfd4(int fd, const void *mask, size_t sigsetsize, int flags) {
    if (!mask) return -EFAULT;
    if (sigsetsize != ARCH_SIGSET_SIZE) return -EINVAL;
    arch_sigset_t user_mask;
    if (copy_from_user(&user_mask, mask, sizeof(user_mask)) < 0)
        return -EFAULT;
    uint64_t kernel_mask = arch_user_sigset_to_kernel(&user_mask);
    return signalfd_create(fd, kernel_mask, flags);
}
