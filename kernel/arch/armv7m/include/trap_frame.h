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
    uint32_t kernel_tp;
    uint32_t control;
} trap_context_t;

typedef struct {
    uint32_t r4_r11[8];
    uint32_t sp;
    uint32_t ra;
    uint32_t tp;
    uint32_t control;
    uint32_t page_table;
    uint32_t status;
} task_context_t;

/* Architecture-private task state used by exception-return preemption. */
#define ARCH_TASK_FIELDS \
    uintptr_t arch_preempt_resume_pc; \
    uint32_t arch_preempt_resume_xpsr; \
    uint32_t arch_preempt_active; \
    uint32_t arch_preempt_disable;

#define ARCH_TASK_INIT(task) do { \
    (task)->arch_preempt_resume_pc = 0; \
    (task)->arch_preempt_resume_xpsr = 0; \
    (task)->arch_preempt_active = 0; \
    (task)->arch_preempt_disable = 0; \
} while (0)

#define ARCH_SCHED_ENTER(task) do { \
    if (task) (task)->arch_preempt_disable++; \
} while (0)

#define ARCH_SCHED_LEAVE(task) do { \
    if ((task) && (task)->arch_preempt_disable) \
        (task)->arch_preempt_disable--; \
} while (0)

#define ARCH_IDLE_CONTEXT_STATIC(name, count) \
    static task_context_t name[count] __attribute__((aligned(8)))
#define ARCH_IDLE_STACK(contexts, cpu) ((void *)&(contexts)[cpu])
#define ARCH_IDLE_STACK_INIT(stack) do { (void)(stack); } while (0)
#define ARCH_IDLE_STACK_TOP(stack) ({ \
    uintptr_t __top; \
    (void)(stack); \
    __asm__ __volatile__("mov %0, sp" : "=r"(__top)); \
    __top; \
})

typedef struct {
    uint32_t r[16];
    uint32_t xpsr;
} arch_sigcontext_t;

#define ARCH_UCONTEXT_PAD_FIELDS uint32_t uc_pad;
#define ARCH_SIGFRAME_EXTRA_FIELDS uint32_t arch_extra;
#define TRAP_CONTEXT_SIZE ((13 + 7) * 4)
#define TASK_CONTEXT_SIZE ((8 + 6) * 4)
#define KTRAP_CONTEXT_SIZE TRAP_CONTEXT_SIZE
#define ARCH_SYSCALL_TRACE_MIN_PID 1

#define ARCH_TASK_CONTEXT_SET_USER_TP(ctx, value) do { (void)(ctx); (void)(value); } while (0)

extern void __switch(uint32_t next_kstack);
extern void user_trap_return(void);
extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);

_Static_assert(sizeof(trap_context_t) == TRAP_CONTEXT_SIZE,
               "ARMv7-M trap context layout mismatch");
_Static_assert(sizeof(task_context_t) == TASK_CONTEXT_SIZE,
               "ARMv7-M task context layout mismatch");

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
    (void)top;
    (void)trap;
    /* On ARMv7-M the task_context_t lives at the bottom of the kernel stack
     * so the C call stack growing downward from the top does not overwrite it.
     * __switch resumes using ctx->sp, not the context base. */
    return (task_context_t *)base;
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

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc,
                                              const trap_context_t *ctx) {
    for (int i = 0; i < 13; i++)
        sc->r[i] = ctx->r[i];
    sc->r[13] = ctx->sp;
    sc->r[14] = ctx->lr;
    sc->r[15] = ctx->pc;
    sc->xpsr = ctx->xpsr;
}

static inline void arch_signal_build_frame_extra(void *extra,
                                                  const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0x0000df01U;
    tramp[1] = 0x0000df00U;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0x0000df01U;
    p[1] = 0x0000df00U;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx,
                                                 const arch_sigcontext_t *sc) {
    for (int i = 0; i < 13; i++)
        ctx->r[i] = sc->r[i];
    ctx->sp = sc->r[13];
    ctx->lr = sc->r[14];
    ctx->pc = sc->r[15];
    ctx->xpsr = sc->xpsr;
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx,
                                                    const void *extra) {
    (void)ctx;
    (void)extra;
}

#endif
