/*
 * Native syscall entry for the RV32 M-mode flat-program profile.
 *
 * The generic dispatcher (kernel/syscall/syscall.c) is not part of the MCU
 * trim: it carries KEP, seccomp and signal delivery, none of which exist in
 * this footprint.  What the profile does share with every other A20 ABI is
 * the generated syscall table, so dispatch here is a single lookup against
 * exactly the syscalls the CMSIS manifest declares.
 */
#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "core/types.h"
#include "core/arch.h"
#include "core/errno.h"
#include "proc/proc.h"
#include "trap_frame.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"

extern int64_t riscv_hi_vendor_call(uint64_t nr, trap_context_t *ctx);
extern int riscv_hi_vendor_is_call(uint64_t nr);

/*
 * Consume a reschedule request before returning to U-mode.  Without this the
 * only safe point on the MCU path is the next timer tick, so a thread woken
 * by futex (a CMSIS semaphore release, a mutex handoff) would wait up to a
 * full tick even when it outranks the caller.  The trap frame and sepc are
 * already committed, so yielding here resumes at the instruction after ecall.
 */
static inline void mcu_syscall_return(int64_t *ret, trap_context_t *ctx)
{
    TRAP_CTX_SET_RET(ctx, *ret);
    arch_advance_syscall_epc(ctx);
    (void)proc_sched_safe_point();
}

int64_t riscv_mcu_syscall(trap_context_t *ctx)
{
    uint64_t nr = TRAP_CTX_SYSCALL_NUM(ctx);

    /* Vendor extensions have their own range and never enter the standard
     * table; the CMSIS manifest therefore cannot accidentally grow them. */
    if (riscv_hi_vendor_is_call(nr)) {
        int64_t vret = riscv_hi_vendor_call(nr, ctx);
        mcu_syscall_return(&vret, ctx);
        return vret;
    }
    const a20_syscall_entry_t *entry = a20_syscall_lookup(nr);
    int64_t ret = -A20_ERR_NOT_SUPPORTED;

    if (entry) {
        a20_syscall_args_t args = {
            .nr = nr,
            .arg = {
                TRAP_CTX_ARG0(ctx),
                TRAP_CTX_ARG1(ctx),
                TRAP_CTX_ARG2(ctx),
                TRAP_CTX_ARG3(ctx),
                TRAP_CTX_ARG4(ctx),
                TRAP_CTX_ARG5(ctx),
            },
            .ctx = ctx,
        };
        ret = entry->handler(&args);
    }

    /* A thread_exit never reaches here: proc_exit() does not return. */
    mcu_syscall_return(&ret, ctx);
    return ret;
}
#endif
