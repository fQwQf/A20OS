#include "core/trap.h"
#include "core/defs.h"

extern void trap_handler(trap_context_t *ctx);
extern void kernel_trap_handler(trap_context_t *ctx);
extern char loongarch64_idle_region_start[];
extern char loongarch64_idle_region_end[];

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
    uint64_t cause = arch_read_cause();
    uint64_t pplv = ctx->prmd & 0x3;

    /*
     * Ecode 0 with no ECFG-enabled IS bit pending is a spurious interrupt:
     * the exception was recognized, but by the time ESTAT was read the
     * source was already gone.  Ecode 0 still makes it interrupt-class, so
     * the idle fixup applies, but it must not be dispatched — neither
     * generic handler accepts cause 0 (user_trap_handler would kill the
     * task, kernel_trap_handler would panic "Unhandled kernel trap").
     */
    if (cause == 0) {
        uintptr_t start = (uintptr_t)loongarch64_idle_region_start;
        uintptr_t end = (uintptr_t)loongarch64_idle_region_end;
        if (ctx->era >= start && ctx->era < end)
            ctx->era = end;
        return;
    }

    if (pplv != 0) {
        trap_handler(ctx);
    } else {
        uintptr_t start = (uintptr_t)loongarch64_idle_region_start;
        uintptr_t end = (uintptr_t)loongarch64_idle_region_end;
        if ((cause >> 63) && ctx->era >= start && ctx->era < end)
            ctx->era = end;
        kernel_trap_handler(ctx);
    }
}
