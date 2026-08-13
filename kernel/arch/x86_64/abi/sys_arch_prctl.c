#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "abi/linux/syscall_impl.h"
#include "proc/proc.h"
#include "trap_frame.h"
#include "sys/usercopy.h"

#define ARCH_SET_GS      0x1001
#define ARCH_SET_FS      0x1002
#define ARCH_GET_FS      0x1003
#define ARCH_GET_GS      0x1004
#define ARCH_GET_CPUID   0x1011
#define ARCH_SET_CPUID   0x1012

/*
 * arch_prctl(2) on x86_64.
 *
 * FS base is the user-space TLS pointer and is carried in the trap frame
 * (restored to MSR_FS_BASE on every return to user).  GS base is reserved on
 * A20OS for the kernel's per-CPU data pointer (MSR_GS_BASE), so the user GS
 * value requested through ARCH_SET_GS is kept per task and reported back by
 * ARCH_GET_GS; user code that wants a GS segment must use FS or a memory
 * model that does not depend on GS.  ARCH_GET_CPUID reports CPUID as enabled;
 * disabling CPUID (ARCH_SET_CPUID with 0) is refused with -EINVAL because the
 * kernel does not trap the CPUID instruction.
 */

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
            t->user_gs_base = addr;
            return 0;
        case ARCH_GET_GS:
            if (copy_to_user((void *)addr, &t->user_gs_base, sizeof(uint64_t)) < 0)
                return -EFAULT;
            return 0;
        case ARCH_GET_CPUID: {
            uint64_t one = 1;
            if (copy_to_user((void *)addr, &one, sizeof(one)) < 0)
                return -EFAULT;
            return 0;
        }
        case ARCH_SET_CPUID:
            return -EINVAL; /* CPUID is always enabled; cannot be disabled */
        default:
            return -EINVAL;
    }
}
