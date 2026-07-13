#ifdef CONFIG_PPC64LE

#include "core/trap.h"
#include "core/stdio.h"
#include "proc/proc.h"
#include "asm/ppc64-regs.h"

void ppc64_trap_dispatch(trap_context_t *ctx)
{
    volatile uint64_t *scratch =
        (volatile uint64_t *)(PAGE_OFFSET + PPC64_TRAP_SCRATCH_PA);
    task_t *task = proc_current();
    uint64_t syscall_num = ctx->gpr[0];
    trap_handler(ctx);
    if (scratch[6] == CAUSE_ECALL_U) {
        /*
         * The PPC64 Linux syscall ABI reports errors with CR0.SO in addition
         * to returning a positive errno in userspace after libc's negation.
         * Preserve the other CR fields and update only the syscall summary
         * overflow bit.
         */
        const uint64_t cr0_so = 0x10000000UL;
        if ((int64_t)ctx->gpr[3] < 0) {
            ctx->gpr[3] = (uint64_t)-(int64_t)ctx->gpr[3];
            ctx->cr |= cr0_so;
        } else {
            ctx->cr &= ~cr0_so;
        }
        task = proc_current();
        if (syscall_num == 220 || syscall_num == 221 || syscall_num == 260)
            printf("[PPC64-SYSCALL] pid=%d nr=%lu ret=%ld\n",
                   task ? task->pid : -1, (unsigned long)syscall_num,
                   (long)ctx->gpr[3]);
    }
    /*
     * Some pseries exception paths report SRR1 without SF even though the
     * interrupted ELFv2 task is 64-bit.  Returning with SF clear truncates
     * the next user branch target to 32 bits.
     */
    ctx->msr |= PPC64_MSR_SF | PPC64_MSR_ISF;
}

#endif
