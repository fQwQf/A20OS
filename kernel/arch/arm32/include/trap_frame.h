#ifndef _ARCH_ARM32_TRAP_H
#define _ARCH_ARM32_TRAP_H

#include "core/types.h"
#include "proc/debug_regs.h"
#include "page_table.h"
#include "platform.h"

typedef struct {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
    uint32_t kernel_tp;
    uint32_t kernel_sp;
    uint32_t ttbr0;
    uint32_t fault_addr;
} trap_context_t;

_Static_assert(sizeof(trap_context_t) == 84, "trap_context_t must stay in sync with trap.S");
_Static_assert(__builtin_offsetof(trap_context_t, sp) == 52, "trap_context_t.sp offset mismatch");
_Static_assert(__builtin_offsetof(trap_context_t, lr) == 56, "trap_context_t.lr offset mismatch");
_Static_assert(__builtin_offsetof(trap_context_t, pc) == 60, "trap_context_t.pc offset mismatch");
_Static_assert(__builtin_offsetof(trap_context_t, cpsr) == 64, "trap_context_t.cpsr offset mismatch");
_Static_assert(__builtin_offsetof(trap_context_t, kernel_tp) == 68, "trap_context_t.kernel_tp offset mismatch");
_Static_assert(__builtin_offsetof(trap_context_t, kernel_sp) == 72, "trap_context_t.kernel_sp offset mismatch");
_Static_assert(__builtin_offsetof(trap_context_t, ttbr0) == 76, "trap_context_t.ttbr0 offset mismatch");
_Static_assert(__builtin_offsetof(trap_context_t, fault_addr) == 80, "trap_context_t.fault_addr offset mismatch");

typedef struct {
    uint32_t ra;
    uint32_t tp;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t sp;
    uint32_t ttbr0;
    uint32_t cpsr;
    uint32_t user_tp;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 56, "task_context_t must stay in sync with switch.S");
_Static_assert(__builtin_offsetof(task_context_t, sp) == 40, "task_context_t.sp offset mismatch");
_Static_assert(__builtin_offsetof(task_context_t, ttbr0) == 44, "task_context_t.ttbr0 offset mismatch");
_Static_assert(__builtin_offsetof(task_context_t, cpsr) == 48, "task_context_t.cpsr offset mismatch");
_Static_assert(__builtin_offsetof(task_context_t, user_tp) == 52, "task_context_t.user_tp offset mismatch");

typedef struct {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
} arch_sigcontext_t;

#define ARCH_UCONTEXT_PAD_FIELDS uint32_t uc_pad;
#define ARCH_SIGFRAME_EXTRA_FIELDS uint32_t arch_extra;

#define TRAP_CONTEXT_SIZE  ((13 + 8) * 4)
#define TASK_CONTEXT_SIZE  (14 * 4)
#define KTRAP_CONTEXT_SIZE TRAP_CONTEXT_SIZE
#define ARCH_SYSCALL_TRACE_MIN_PID 3

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint64_t next_kstack);
extern void user_trap_return(void);
extern void arm32_vector_table(void);

#define TRAP_CTX_SYSCALL_NUM(ctx)  ((ctx)->r[7])
#define TRAP_CTX_ARG0(ctx)         ((ctx)->r[0])
#define TRAP_CTX_ARG1(ctx)         ((ctx)->r[1])
#define TRAP_CTX_ARG2(ctx)         ((ctx)->r[2])
#define TRAP_CTX_ARG3(ctx)         ((ctx)->r[3])
#define TRAP_CTX_ARG4(ctx)         ((ctx)->r[4])
#define TRAP_CTX_ARG5(ctx)         ((ctx)->r[5])
#define TRAP_CTX_RET(ctx)          ((ctx)->r[0])
#define TRAP_CTX_SP(ctx)           ((ctx)->sp)
#define TRAP_CTX_RA(ctx)           ((ctx)->lr)
#define TRAP_CTX_FP(ctx)           ((ctx)->r[11])
#define TRAP_CTX_TP(ctx)           ((ctx)->fault_addr)

#define TRAP_CTX_SET_RET(ctx, v)   do { (ctx)->r[0] = (uint32_t)(v); } while (0)
#define TRAP_CTX_SET_ARG0(ctx, v)  do { (ctx)->r[0] = (uint32_t)(v); } while (0)
#define TRAP_CTX_SET_SP(ctx, v)    do { (ctx)->sp = (uint32_t)(v); } while (0)
static inline uint64_t arch_trap_ctx_reg(const trap_context_t *ctx, int i) {
    if (i >= 0 && i < 13)
        return ctx->r[i];
    if (i == 13)
        return ctx->sp;
    if (i == 14)
        return ctx->lr;
    if (i == 15)
        return ctx->pc;
    return 0;
}
static inline void arch_trap_ctx_set_reg(trap_context_t *ctx, int i, uint64_t v) {
    if (i >= 0 && i < 13)
        ctx->r[i] = (uint32_t)v;
    else if (i == 13)
        ctx->sp = (uint32_t)v;
    else if (i == 14)
        ctx->lr = (uint32_t)v;
    else if (i == 15)
        ctx->pc = (uint32_t)v;
}
#define TRAP_CTX_REG(ctx, i)       arch_trap_ctx_reg((ctx), (i))
#define TRAP_CTX_SET_REG(ctx, i, v) arch_trap_ctx_set_reg((ctx), (i), (v))

#define TRAP_CTX_EPC(ctx)          ((ctx)->pc)
#define TRAP_CTX_STATUS(ctx)       ((ctx)->cpsr)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->ttbr0)
#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->ttbr0)
#define TASK_CTX_STATUS(ctx)       ((ctx)->cpsr)
#define ARCH_TASK_CONTEXT_SET_USER_TP(ctx, value) \
    do { (ctx)->user_tp = (uint32_t)(value); } while (0)

static inline void arch_task_context_set_initial_sp(task_context_t *ctx, trap_context_t *trap, uint64_t stack_top) {
    /*
     * Keep the live SVC stack below the persistent trap frame at the top of
     * each user task's kernel stack.  Starting at stack_top makes the first
     * 88-byte exception frame overlap trap_context_t and corrupts saved user
     * registers during fork/exec-heavy startup.
     */
    ctx->sp = trap ? trap->kernel_sp : (uint32_t)stack_top;
}

static inline task_context_t *arch_task_context_base(void *kstack_base, uint64_t stack_top, trap_context_t *trap) {
    (void)stack_top;
    (void)trap;
    /* Keep the bootstrap task context out of the live SVC call stack. */
    return (task_context_t *)kstack_base;
}

static inline uint64_t arch_task_kernel_status(void) {
    return SSTATUS_SIE | 0x13U; /* SVC mode, IRQs disabled */
}

static inline uint64_t arch_user_initial_status(void) {
    return 0x10U;
}

static inline void arch_trap_ctx_set_user_entry(trap_context_t *ctx,
                                                uint64_t entry) {
    TRAP_CTX_EPC(ctx) = entry;
}

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0xe3a070adU; /* mov r7, #173 (rt_sigreturn) */
    tramp[1] = 0xef000000U; /* svc 0 */
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0xe3a070adU;
    p[1] = 0xef000000U;
}

static inline uint64_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D | PTE_LEAF;
}

static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint64_t ksp) {
    ctx->kernel_sp = (uint32_t)(ksp - 88U);
}

static inline uint64_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx, uint64_t fallback) {
    return ctx->kernel_sp ? (uint64_t)(ctx->kernel_sp + 88U) : fallback;
}

static inline void arch_advance_syscall_epc(trap_context_t *ctx) {
    TRAP_CTX_EPC(ctx) += 4;
}

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc, const trap_context_t *ctx) {
    for (int i = 0; i < 13; i++)
        sc->r[i] = ctx->r[i];
    sc->sp = ctx->sp;
    sc->lr = ctx->lr;
    sc->pc = ctx->pc;
    sc->cpsr = ctx->cpsr;
}

static inline void arch_signal_build_frame_extra(void *extra, const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx, const arch_sigcontext_t *sc) {
    for (int i = 0; i < 13; i++)
        ctx->r[i] = sc->r[i];
    ctx->sp = sc->sp;
    ctx->lr = sc->lr;
    ctx->pc = sc->pc;
    ctx->cpsr = sc->cpsr;
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx, const void *extra) {
    (void)ctx;
    (void)extra;
}


/* ---- debugging interface (kernel/proc/debug.c) ---- */
static inline void arch_ptrace_rewind_syscall(trap_context_t *ctx) {
    (void)ctx;
}
static inline void arch_ptrace_set_step(trap_context_t *ctx) {
    (void)ctx;
}
static inline void arch_ptrace_export_regs(const trap_context_t *ctx,
                                           proc_debug_regs_t *out) {
    (void)ctx;
    (void)out;
}
static inline void arch_ptrace_import_regs(trap_context_t *ctx,
                                           const proc_debug_regs_t *in) {
    (void)ctx;
    (void)in;
}
#endif
