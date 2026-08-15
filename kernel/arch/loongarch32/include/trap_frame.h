#ifndef _ARCH_LOONGARCH32_TRAP_H
#define _ARCH_LOONGARCH32_TRAP_H

#include "core/types.h"
#include "proc/debug_regs.h"
#include "page_table.h"
#include "platform.h"

/*
 * NaiLoong Core has no floating-point / LSX unit, so the trap frame carries
 * only the 32 GPRs plus the exception metadata.
 */
typedef struct {
    uint32_t regs[32];
    uint32_t era;
    uint32_t prmd;
    uint32_t kernel_tp;
    uint32_t kernel_sp;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 36 * 4, "TrapContext must be 144 bytes");

typedef struct {
    uint32_t ra;
    uint32_t tp;
    uint32_t s[9];
    uint32_t fp;
    uint32_t sp;
    uint32_t pgdl;
    uint32_t prmd;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 15 * 4, "TaskContext must be 60 bytes");

typedef struct {
    uint32_t sc_pc;
    uint32_t sc_regs[32];
    uint32_t sc_flags;
} __attribute__((aligned(16))) arch_sigcontext_t;

#define ARCH_UCONTEXT_PAD_FIELDS uint32_t uc_pad;
#define ARCH_SIGFRAME_EXTRA_FIELDS uint32_t arch_extra;

#define TRAP_CONTEXT_SIZE  (36 * 4)
#define TASK_CONTEXT_SIZE  (15 * 4)
#define KTRAP_CONTEXT_SIZE (36 * 4)
#define ARCH_SYSCALL_TRACE_MIN_PID 3

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint32_t next_kstack);
extern void user_trap_return(void);
extern void trap_handler_la32(trap_context_t *ctx);

/* Syscall register mapping (LoongArch ABI):
 *   $a7 = $r11 = syscall number
 *   $a0-$a5 = $r4-$r9 = arguments
 */
#define TRAP_CTX_SYSCALL_NUM(ctx)  ((ctx)->regs[11])
#define TRAP_CTX_ARG0(ctx)        ((ctx)->regs[4])
#define TRAP_CTX_ARG1(ctx)        ((ctx)->regs[5])
#define TRAP_CTX_ARG2(ctx)        ((ctx)->regs[6])
#define TRAP_CTX_ARG3(ctx)        ((ctx)->regs[7])
#define TRAP_CTX_ARG4(ctx)        ((ctx)->regs[8])
#define TRAP_CTX_ARG5(ctx)        ((ctx)->regs[9])
#define TRAP_CTX_RET(ctx)         ((ctx)->regs[4])
#define TRAP_CTX_SP(ctx)          ((ctx)->regs[3])
#define TRAP_CTX_RA(ctx)          ((ctx)->regs[1])
#define TRAP_CTX_TP(ctx)          ((ctx)->regs[2])
#define TRAP_CTX_FP(ctx)          ((ctx)->regs[22])

#define TRAP_CTX_SET_RET(ctx, v)  do { (ctx)->regs[4] = (uint32_t)(v); } while(0)
#define TRAP_CTX_SET_ARG0(ctx, v) do { (ctx)->regs[4] = (uint32_t)(v); } while(0)
#define TRAP_CTX_SET_SP(ctx, v)   do { (ctx)->regs[3] = (uint32_t)(v); } while(0)
#define TRAP_CTX_SET_REG(ctx, i, v) do { (ctx)->regs[i] = (uint32_t)(v); } while(0)
#define TRAP_CTX_REG(ctx, i)      ((ctx)->regs[i])

#define TRAP_CTX_EPC(ctx)          ((ctx)->era)
#define TRAP_CTX_STATUS(ctx)       ((ctx)->prmd)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->regs[0])

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->pgdl)
#define TASK_CTX_STATUS(ctx)       ((ctx)->prmd)

static inline void arch_task_context_set_initial_sp(task_context_t *ctx,
                                                     trap_context_t *trap,
                                                     uint32_t stack_top) {
    (void)ctx;
    (void)trap;
    (void)stack_top;
}

static inline task_context_t *arch_task_context_base(void *kstack_base,
                                                      uint32_t stack_top,
                                                      trap_context_t *trap) {
    (void)kstack_base;
    if (trap)
        return (task_context_t *)((uintptr_t)trap - sizeof(task_context_t));
    return (task_context_t *)(uintptr_t)(stack_top - sizeof(task_context_t));
}

static inline uint32_t arch_task_kernel_status(void) {
    return SSTATUS_SIE;
}

static inline uint32_t arch_user_initial_status(void) {
    return SSTATUS_SPIE | SSTATUS_FS_CLEAN;
}

static inline void arch_trap_ctx_set_user_entry(trap_context_t *ctx,
                                                uint32_t entry) {
    TRAP_CTX_EPC(ctx) = entry;
}

/* LoongArch syscall instruction (syscall 0) used as the signal trampoline.
 * The rt_sigreturn syscall number is loaded into $a0 first. */
static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0x02822c0b;
    tramp[1] = 0x002b0000;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0x02822c0b;
    p[1] = 0x002b0000;
}

static inline uint32_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_D | PTE_MAT1 | PTE_LEAF;
}

static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint32_t ksp) {
    ctx->kernel_sp = ksp;
}

static inline uint32_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx, uint32_t fallback) {
    (void)fallback;
    return ctx->kernel_sp;
}

static inline void arch_advance_syscall_epc(trap_context_t *ctx) {
    TRAP_CTX_EPC(ctx) += 4;
}

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc,
                                              const trap_context_t *ctx) {
    sc->sc_pc = ctx->era;
    for (int i = 0; i < 32; i++)
        sc->sc_regs[i] = ctx->regs[i];
    sc->sc_flags = 1U;
}

static inline void arch_signal_build_frame_extra(void *extra,
                                                 const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx,
                                                const arch_sigcontext_t *sc) {
    for (int i = 0; i < 32; i++)
        ctx->regs[i] = sc->sc_regs[i];
    ctx->era = sc->sc_pc;
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx,
                                                   const void *extra) {
    (void)ctx;
    (void)extra;
}

/* ---- debugging interface (kernel/proc/debug.c) ---- */

static inline void arch_ptrace_rewind_syscall(trap_context_t *ctx) {
    TRAP_CTX_EPC(ctx) -= 4;
}

static inline void arch_ptrace_set_step(trap_context_t *ctx) {
    (void)ctx;
}

static inline void arch_ptrace_export_regs(const trap_context_t *ctx,
                                           proc_debug_regs_t *out) {
    for (int i = 0; i < 32; i++)
        out->regs[i] = ctx->regs[i];
    out->pc = ctx->era;
    out->sp = ctx->regs[3];
    out->status = ctx->prmd;
    out->orig_syscall = ctx->regs[11];
}

static inline void arch_ptrace_import_regs(trap_context_t *ctx,
                                           const proc_debug_regs_t *in) {
    for (int i = 0; i < 32; i++)
        ctx->regs[i] = in->regs[i];
    ctx->era = in->pc;
    ctx->regs[3] = in->sp;
    ctx->prmd = in->status;
    ctx->regs[11] = in->orig_syscall;
}

#endif
