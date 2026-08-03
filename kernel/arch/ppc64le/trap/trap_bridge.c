#ifdef CONFIG_PPC64LE

#include "core/trap.h"
#include "core/stdio.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "asm/ppc64-regs.h"

/*
 * Updated by __return_to_user with the user stack pointer that is about to be
 * restored.  On POWER10 pSeries the storage faults re-routed below are taken
 * with r1 already clobbered to the TIMA scratch base (0x1000) by the time the
 * vector runs, so the trap frame's saved r1 is lost; recover it from here.
 */
volatile uint64_t ppc64_rtu_r1;
volatile uint64_t ppc64_rtu_r2;
volatile uint64_t ppc64_rtu_nip;

void ppc64_trap_dispatch(trap_context_t *ctx)
{
    volatile uint64_t *scratch =
        (volatile uint64_t *)(PAGE_OFFSET + PPC64_TRAP_SCRATCH_PA);
    task_t *task = proc_current();
    uint64_t syscall_num = ctx->gpr[0];
    /*
     * On POWER10 pSeries the guest-facing XIVE controller delivers storage
     * (page-fault) exceptions to the external vector (0x500) instead of the
     * DSI/ISI vectors.  Instruction faults carry the 0x40000000 SRR1 error
     * bit and the faulting EA in SRR0; data faults carry no SRR1 error bit
     * and the faulting EA in DAR.  Re-route both to the normal page-fault
     * paths.  The port has no real external-interrupt device: every driver
     * polls and the decrementer uses the separate 0x900 exception.
     */
    if ((scratch[6] & CAUSE_CODE_MASK) == IRQ_S_EXT) {
        if (ctx->gpr[1] == (uint64_t)PPC64_TRAP_SCRATCH_PA &&
            ppc64_rtu_r1)
            ctx->gpr[1] = ppc64_rtu_r1;
        if (ppc64_rtu_r2)
            ctx->gpr[2] = ppc64_rtu_r2;
        /* QEMU 10.0.11 clobbers SRR0 to the trap-entry address for both
         * instruction and data storage faults; recover the user PC from the
         * last user return target so __return_to_user does not resume in
         * kernel code. */
        {
            uint64_t fa = ctx->nip;
            if ((fa & 0xc000000000000000UL) == 0xc000000000000000UL &&
                ppc64_rtu_nip && !(ppc64_rtu_nip & 0xc000000000000000UL))
                fa = ppc64_rtu_nip;
            ctx->nip = fa;
        }
        if (scratch[5] & 0x40000000UL) {
            /*
             * The QEMU 10.0.11 0x500 delivery for an instruction fetch
             * clobbers SRR0 to the trap-entry address.  The real faulting
             * address is the user PC that was just restored (or, after a
             * fault inside the entry path, the last user return target), so
             * fall back to it when SRR0 is a kernel address.
             */
            uint64_t fa = ctx->nip;
            if ((fa & 0xc000000000000000UL) == 0xc000000000000000UL &&
                ppc64_rtu_nip && !(ppc64_rtu_nip & 0xc000000000000000UL))
                fa = ppc64_rtu_nip;
            scratch[6] = CAUSE_INSN_PAGE_FAULT;
            scratch[11] = fa;
            /* The return path reloads the user PC from the trap frame, so the
             * clobbered SRR0 must be corrected here as well. */
            ctx->nip = fa;
        } else {
            uint64_t dsisr, dar;
            __asm__ __volatile__("mfspr %0,18" : "=r"(dsisr));
            __asm__ __volatile__("mfspr %0,19" : "=r"(dar));
            /*
             * QEMU 10.0.11 sometimes delivers an instruction-fetch storage
             * fault to the external vector without the SRR1 instruction bit;
             * DAR is then clobbered to 0 and only the recovered user PC is
             * meaningful.  Treat such faults as instruction page faults.
             */
            if (dar == 0 && ppc64_rtu_nip &&
                !(ppc64_rtu_nip & 0xc000000000000000UL)) {
                scratch[6] = CAUSE_INSN_PAGE_FAULT;
                scratch[11] = ppc64_rtu_nip;
            } else {
                scratch[6] = (dsisr & 0x02000000UL) ? CAUSE_STORE_PAGE_FAULT
                                                    : CAUSE_LOAD_PAGE_FAULT;
                scratch[11] = dar;
            }
        }
    }
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
