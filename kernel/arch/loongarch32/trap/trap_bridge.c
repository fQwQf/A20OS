#include "core/trap.h"
#include "core/defs.h"

extern void trap_handler(trap_context_t *ctx);
extern void kernel_trap_handler(trap_context_t *ctx);

/*
 * Route to the correct handler based on the privilege level at the time of
 * the exception/interrupt (PRMD.PPLV), NOT on whether the current task has
 * an mm: a user task can take an interrupt while running in kernel mode
 * (syscall / right after scheduling).
 */
void trap_handler_la32(trap_context_t *ctx) {
    uint32_t pplv = ctx->prmd & 0x3;

    if (pplv != 0) {
        trap_handler(ctx);
    } else {
        kernel_trap_handler(ctx);
    }
}
