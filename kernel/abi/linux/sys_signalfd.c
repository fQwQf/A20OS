#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "ipc/signalfd.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"

int64_t sys_signalfd4(int fd, const void *mask, size_t sigsetsize, int flags) {
    if (!mask) return -EFAULT;
    if (sigsetsize != ARCH_SIGSET_SIZE) return -EINVAL;
    arch_sigset_t user_mask;
    if (copy_from_user(&user_mask, mask, sizeof(user_mask)) < 0)
        return -EFAULT;
    uint64_t kernel_mask = arch_user_sigset_to_kernel(&user_mask);
    if (fd >= 0)
        return signalfd_create(fd, kernel_mask, flags);
    int ufd = signalfd_create(-1, kernel_mask, flags);
    if (ufd < 0)
        return ufd;
    int gfd = fdtable_get_current(ufd);
    env_kind_register(gfd, A20_OBJ_EVENT_QUEUE);
    if (env_active(proc_current())) {
        uint64_t rights = A20_RIGHT_READ | A20_RIGHT_STAT;
        int mr = env_mediate_acquire((uint8_t)A20_OBJ_EVENT_QUEUE,
                                     rights, gfd);
        if (mr) {
            fdtable_close_current(ufd);
            return mr;
        }
    }
    return ufd;
}
