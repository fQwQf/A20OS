#include "trap.h"
#include "proc.h"

extern void trap_handler(trap_context_t *ctx);
extern void kernel_trap_handler(trap_context_t *ctx);

void trap_handler_la64(trap_context_t *ctx) {
    task_t *cur = proc_current();
    if (cur && cur->mm) {
        trap_handler(ctx);
    } else {
        kernel_trap_handler(ctx);
    }
}
