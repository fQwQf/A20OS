#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "abi/linux/syscall_impl.h"
#include "proc/proc.h"
#include "trap_frame.h"
#include "sys/usercopy.h"

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

int64_t sys_arch_prctl(int op, uint64_t addr)
{
    task_t *t = proc_current();
    if (!t || !t->trap_ctx)
        return -ESRCH;

    switch (op) {
        case ARCH_SET_FS:
            TRAP_CTX_TP(t->trap_ctx) = addr;
            return 0;
        case ARCH_GET_FS:
            if (copy_to_user((void *)addr, &TRAP_CTX_TP(t->trap_ctx), sizeof(uint64_t)) < 0)
                return -EFAULT;
            return 0;
        case ARCH_SET_GS:
        case ARCH_GET_GS:
            return -ENOSYS;
        default:
            return -EINVAL;
    }
}
