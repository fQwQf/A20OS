#ifndef _ARCH_ARMV7M_TRAP_FRAME_H
#define _ARCH_ARMV7M_TRAP_FRAME_H

#include "core/types.h"
#include "page_table.h"

typedef struct {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t xpsr;
    uint32_t kernel_sp;
    uint32_t control;
} trap_context_t;

typedef struct {
    uint32_t r4_r11[8];
    uint32_t sp;
    uint32_t lr;
    uint32_t control;
    uint32_t page_table;
    uint32_t status;
} task_context_t;

typedef struct {
    uint32_t r[16];
    uint32_t xpsr;
} arch_sigcontext_t;

#define ARCH_UCONTEXT_PAD_FIELDS uint32_t uc_pad;
#define ARCH_SIGFRAME_EXTRA_FIELDS uint32_t arch_extra;
#define TRAP_CONTEXT_SIZE ((13 + 6) * 4)
#define TASK_CONTEXT_SIZE ((8 + 5) * 4)
#define KTRAP_CONTEXT_SIZE TRAP_CONTEXT_SIZE
#define ARCH_SYSCALL_TRACE_MIN_PID 1

extern void __switch(uint32_t next_kstack);
extern void user_trap_return(void);
extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);

#define TRAP_CTX_SYSCALL_NUM(ctx) ((ctx)->r[7])
#define TRAP_CTX_ARG0(ctx) ((ctx)->r[0])
#define TRAP_CTX_ARG1(ctx) ((ctx)->r[1])
#define TRAP_CTX_ARG2(ctx) ((ctx)->r[2])
#define TRAP_CTX_ARG3(ctx) ((ctx)->r[3])
#define TRAP_CTX_ARG4(ctx) ((ctx)->r[4])
#define TRAP_CTX_ARG5(ctx) ((ctx)->r[5])
#define TRAP_CTX_RET(ctx) ((ctx)->r[0])
#define TRAP_CTX_SP(ctx) ((ctx)->sp)
#define TRAP_CTX_RA(ctx) ((ctx)->lr)
#define TRAP_CTX_FP(ctx) ((ctx)->r[7])
#define TRAP_CTX_TP(ctx) ((ctx)->r[9])
#define TRAP_CTX_EPC(ctx) ((ctx)->pc)
#define TRAP_CTX_STATUS(ctx) ((ctx)->xpsr)
#define TRAP_CTX_KScratch0(ctx) ((ctx)->control)
#define TASK_CTX_PAGE_TABLE(ctx) ((ctx)->page_table)
#define TASK_CTX_STATUS(ctx) ((ctx)->status)

#define TRAP_CTX_SET_RET(ctx, value) do { (ctx)->r[0] = (uint32_t)(value); } while (0)
#define TRAP_CTX_SET_ARG0(ctx, value) do { (ctx)->r[0] = (uint32_t)(value); } while (0)
#define TRAP_CTX_SET_SP(ctx, value) do { (ctx)->sp = (uint32_t)(value); } while (0)
#define TRAP_CTX_REG(ctx, i) ((i) < 13 ? (ctx)->r[(i)] : 0U)
#define TRAP_CTX_SET_REG(ctx, i, value) \
    do { if ((i) < 13) (ctx)->r[(i)] = (uint32_t)(value); } while (0)

static inline void arch_task_context_set_initial_sp(task_context_t *ctx,
                                                     trap_context_t *trap,
                                                     uint64_t stack_top) {
    ctx->sp = trap ? (uint32_t)(uintptr_t)trap : (uint32_t)stack_top;
}

static inline task_context_t *arch_task_context_base(void *base, uint64_t top,
                                                     trap_context_t *trap) {
    (void)base;
    uintptr_t end = trap ? (uintptr_t)trap : (uintptr_t)top;
    return (task_context_t *)(end - sizeof(task_context_t));
}

static inline uint64_t arch_task_kernel_status(void) { return 0; }
static inline uint64_t arch_user_initial_status(void) { return 0x01000000UL; }
static inline void arch_trap_ctx_set_user_entry(trap_context_t *ctx, uint64_t entry) {
    ctx->pc = (uint32_t)entry | 1U;
    ctx->xpsr = 0x01000000UL;
}
static inline uint64_t arch_signal_tramp_pte_flags(void) { return PTE_USER; }
static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint64_t sp) {
    ctx->kernel_sp = (uint32_t)sp;
}
static inline uint64_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx,
                                                       uint64_t fallback) {
    return ctx->kernel_sp ? ctx->kernel_sp : fallback;
}
static inline void arch_advance_syscall_epc(trap_context_t *ctx) { (void)ctx; }

#endif
