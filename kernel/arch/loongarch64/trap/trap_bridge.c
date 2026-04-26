#include "core/trap.h"
#include "proc/proc.h"

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
    /* PRMD is saved at ctx->prmd; PPLV is bits [1:0] */
    uint64_t pplv = ctx->prmd & 0x3;
    if (pplv != 0) {
        /* Came from user mode (PLV3) */
        trap_handler(ctx);
    } else {
        /* Came from kernel mode (PLV0) */
        kernel_trap_handler(ctx);
    }
}
