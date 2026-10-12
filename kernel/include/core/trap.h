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

/* Native thread_create takes a 16-byte-aligned stack top on every ISA.
 * Architectures with a stack return-address slot reserve its ABI bias before
 * direct function entry. Process startup and Linux clone keep their own rules. */
#ifndef ARCH_NATIVE_THREAD_STACK_BIAS
#define ARCH_NATIVE_THREAD_STACK_BIAS 0U
#endif
static inline uintptr_t arch_native_thread_initial_sp(uintptr_t stack_top) {
    return stack_top - ARCH_NATIVE_THREAD_STACK_BIAS;
}

/* C handlers called from assembly */
void trap_handler(trap_context_t *ctx);
void kernel_trap_handler(trap_context_t *ctx);
/* Return nonzero if guest handling consumed the trap. Called before host
 * frame bookkeeping; the default returns zero and continues host handling. */
int arch_hyp_guest_trap(trap_context_t *ctx);

/* Initialization */
void trap_init(void);
void arch_handle_irq(reg_t irq, int from_user);

/* Architecture-specific cleanup before delivering a user breakpoint. */
void arch_user_breakpoint(trap_context_t *ctx);

#endif /* _TRAP_H */
