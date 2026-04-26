#ifndef _ARCH_AARCH64_TRAP_H
#define _ARCH_AARCH64_TRAP_H

#include "core/types.h"

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t elr;
    uint64_t spsr;
    uint64_t tpidr;
    uint64_t ttbr0;
    uint64_t kernel_tp;
    uint64_t kernel_sp;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 38 * 8, "TrapContext must be 304 bytes");

typedef struct {
    uint64_t ra;
    uint64_t tp;
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t fp;
    uint64_t sp;
    uint64_t ttbr0;
    uint64_t daif;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 16 * 8, "TaskContext must be 128 bytes");

#define TRAP_CONTEXT_SIZE  (38 * 8)
#define TASK_CONTEXT_SIZE  (16 * 8)
#define KTRAP_CONTEXT_SIZE (38 * 8)
#define ARCH_SYSCALL_TRACE_MIN_PID 3

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint64_t next_kstack);
extern void user_trap_return(void);
extern void aarch64_vector_table(void);

#define TRAP_CTX_SYSCALL_NUM(ctx)  ((ctx)->x[8])
#define TRAP_CTX_ARG0(ctx)         ((ctx)->x[0])
#define TRAP_CTX_ARG1(ctx)         ((ctx)->x[1])
#define TRAP_CTX_ARG2(ctx)         ((ctx)->x[2])
#define TRAP_CTX_ARG3(ctx)         ((ctx)->x[3])
#define TRAP_CTX_ARG4(ctx)         ((ctx)->x[4])
#define TRAP_CTX_ARG5(ctx)         ((ctx)->x[5])
#define TRAP_CTX_RET(ctx)          ((ctx)->x[0])
#define TRAP_CTX_SP(ctx)           ((ctx)->sp)
#define TRAP_CTX_RA(ctx)           ((ctx)->x[30])
#define TRAP_CTX_TP(ctx)           ((ctx)->tpidr)

#define TRAP_CTX_SET_RET(ctx, v)   do { (ctx)->x[0] = (uint64_t)(v); } while (0)
#define TRAP_CTX_SET_SP(ctx, v)    do { (ctx)->sp = (uint64_t)(v); } while (0)

#define TRAP_CTX_EPC(ctx)          ((ctx)->elr)
#define TRAP_CTX_STATUS(ctx)       ((ctx)->spsr)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->ttbr0)

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->ttbr0)
#define TASK_CTX_STATUS(ctx)       ((ctx)->daif)

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0xD2801168U; /* movz x8, #139 */
    tramp[1] = 0xD4000001U; /* svc #0 */
}

static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint64_t ksp) {
    ctx->kernel_sp = ksp;
}

static inline uint64_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx, uint64_t fallback) {
    (void)fallback;
    return ctx->kernel_sp;
}

static inline void arch_advance_syscall_epc(trap_context_t *ctx) {
    (void)ctx;
}

#endif
