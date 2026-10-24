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
 * The user FS base lives in MSR_FS_BASE for the whole kernel residency --
 * kernel code never rewrites it, so reads are exact and writes take effect
 * immediately.  The user GS base is parked in MSR_KERNEL_GS_BASE while in
 * kernel mode and becomes active through the SWAPGS on return to user; the
 * trap frame keeps copies for cross-task state (clone inheritance, ptrace).
 * User bases must stay in the lower canonical address range, including when
 * no page is mapped there yet. CPUID disabling is not implemented.
 */

int64_t sys_arch_prctl(int op, uint64_t addr)
{
    task_t *t = proc_current();
    if (!t || !t->trap_ctx)
        return -ESRCH;

    switch (op) {
        case ARCH_SET_FS:
            if (addr >= USER_VA_LIMIT) return -EPERM;
            TRAP_CTX_TP(t->trap_ctx) = addr;
            t->arch_tls_fs_base = addr;
            arch_write_user_fs_base(addr);
            return 0;
        case ARCH_GET_FS:
            /* Read the MSR, not the frame field: the live trap's frame slot
             * is zeroed at entry and only this path refreshes it. */
            {
                uint64_t base = arch_read_user_fs_base();
                if (copy_to_user((void *)addr, &base, sizeof(uint64_t)) < 0)
                    return -EFAULT;
            }
            return 0;
        case ARCH_SET_GS:
            if (addr >= USER_VA_LIMIT) return -EPERM;
            t->trap_ctx->user_gs_base = addr;
            t->arch_tls_gs_base = addr;
            arch_write_user_gs_base(addr);
            return 0;
        case ARCH_GET_GS:
            {
                uint64_t base = arch_read_user_gs_base();
                if (copy_to_user((void *)addr, &base, sizeof(uint64_t)) < 0)
                    return -EFAULT;
            }
            return 0;
        case ARCH_GET_CPUID:
            return 1;
        case ARCH_SET_CPUID:
            return addr == 1 ? 0 : -ENODEV;
        default:
            return -EINVAL;
    }
}
