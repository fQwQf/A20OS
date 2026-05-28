#include "core/trap.h"
#include "core/defs.h"
#include "proc/proc.h"
#include "core/stdio.h"

extern void trap_handler(trap_context_t *ctx);
extern void kernel_trap_handler(trap_context_t *ctx);

/*
 * Route to the correct handler based on the privilege level at the time of
 * the exception/interrupt, NOT on whether the current task has an mm.
 *
 * PRMD.PPLV (bits [1:0]) records the Previous PLV:
 *   3 = PLV3 (user mode)  → user trap handler
 *   0 = PLV0 (kernel mode)→ kernel trap handler
 *
 * Using cur->mm was WRONG: a user task (which has mm) can receive an
 * interrupt while executing in kernel mode (e.g. during a syscall or
 * right after being scheduled).  In that case the EPC points into the
 * kernel and trap_handler's exception-code checks make no sense.
 */
void trap_handler_la64(trap_context_t *ctx) {
    uint64_t pplv = ctx->prmd & 0x3;

    if (pplv != 0) {
        task_t *cur = proc_current();
        if (cur && cur->pid >= 5) {
            uint64_t raw_csr;
            __asm__ __volatile__("csrrd %0, 0x1" : "=r"(raw_csr));
            printf("[BRIDGE] pid=%d ctx=%p prmd_ctx=0x%lx prmd_csr=0x%lx era=0x%lx pplv=%lu off_era=%d off_prmd=%d\n",
                   cur->pid, (void *)ctx,
                   (unsigned long)ctx->prmd,
                   (unsigned long)raw_csr,
                   (unsigned long)ctx->era,
                   (unsigned long)pplv,
                   (int)offsetof(trap_context_t, era),
                   (int)offsetof(trap_context_t, prmd));
        }
        trap_handler(ctx);
    } else {
        kernel_trap_handler(ctx);
    }
}
