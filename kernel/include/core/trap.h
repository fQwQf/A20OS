#ifndef _TRAP_H
#define _TRAP_H

#include "core/types.h"
#include "core/consts.h"
#include "core/arch.h"

/*
 * Architecture-specific types and symbols are now in arch/trap.h,
 * pulled in via arch.h (included from defs.h):
 *   - trap_context_t, task_context_t
 *   - TRAP_CTX_* macros for syscall register access
 *   - __trap_from_user, __return_to_user, __trap_from_kernel, __switch
 *   - user_trap_return
 */

struct task_t;

/* C handlers called from assembly */
void trap_handler(trap_context_t *ctx);
void kernel_trap_handler(trap_context_t *ctx);

int handle_cow_fault(struct task_t *t, uint64_t stval);
int handle_demand_fault(struct task_t *t, uint64_t stval);

/* Initialization */
void trap_init(void);
void arch_handle_irq(uint64_t irq, int from_user);

#endif /* _TRAP_H */
