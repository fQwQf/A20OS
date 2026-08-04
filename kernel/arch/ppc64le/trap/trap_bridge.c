#ifdef CONFIG_PPC64LE

#include "core/trap.h"
#include "core/stdio.h"
#include "proc/proc.h"
#include "asm/ppc64-regs.h"

void ppc64_trap_dispatch(trap_context_t *ctx)
{
    volatile uint64_t *scratch =
        (volatile uint64_t *)(PAGE_OFFSET + PPC64_TRAP_SCRATCH_PA);
    uint64_t cause = scratch[6];

    if ((cause & CAUSE_CODE_MASK) == CAUSE_VPU_UNAVAILABLE) {
        ctx->msr |= PPC64_MSR_VEC | PPC64_MSR_SF | PPC64_MSR_ISF;
        return;
    }

    if ((cause & CAUSE_CODE_MASK) == CAUSE_FP_UNAVAILABLE) {
        /* Retry the faulting scalar-FP instruction with FP enabled. */
        ctx->msr |= PPC64_MSR_FP | PPC64_MSR_SF | PPC64_MSR_ISF;
        return;
    }

    uint64_t syscall_num = ctx->gpr[0];

    trap_handler(ctx);
    if (cause == CAUSE_ECALL_U) {
        /*
         * The PPC64 Linux syscall ABI reports errors with CR0.SO in addition
         * to returning a positive errno in userspace after libc's negation.
         * Preserve the other CR fields and update only the syscall summary
         * overflow bit.
         */
        const uint64_t cr0_so = 0x10000000UL;
        if (syscall_num != 139) {
            if ((int64_t)ctx->gpr[3] < 0) {
                ctx->gpr[3] = (uint64_t)-(int64_t)ctx->gpr[3];
                ctx->cr |= cr0_so;
            } else {
                ctx->cr &= ~cr0_so;
            }
        }
    }
    /*
     * Some pseries exception paths report SRR1 without SF even though the
     * interrupted ELFv2 task is 64-bit.  Returning with SF clear truncates
     * the next user branch target to 32 bits.
     */
    ctx->msr |= PPC64_MSR_SF | PPC64_MSR_ISF;
}

#endif
