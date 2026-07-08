#ifndef _ARCH_RISCV32_TRAP_H
#define _ARCH_RISCV32_TRAP_H

#include "core/types.h"
#include "page_table.h"
#include "platform.h"

typedef struct {
    uint32_t x[32];
    uint32_t sstatus;
    uint32_t sepc;
    uint32_t last_a0;
    uint32_t kernel_tp;
} __attribute__((aligned(16))) trap_context_t;

typedef struct {
    uint32_t ra;
    uint32_t tp;
    uint32_t s[12];
    uint32_t satp;
    uint32_t sstatus;
} task_context_t;

typedef struct {
    uint32_t sc_regs[32];
    uint32_t sc_pc;
    uint32_t sc_status;
} __attribute__((aligned(16))) arch_sigcontext_t;

#define ARCH_UCONTEXT_PAD_FIELDS uint32_t uc_pad;
#define ARCH_SIGFRAME_EXTRA_FIELDS uint32_t arch_extra;

#define TRAP_CONTEXT_SIZE (36 * 4)
#define TASK_CONTEXT_SIZE (16 * 4)
#define KTRAP_CONTEXT_SIZE (36 * 4)
#define ARCH_SYSCALL_TRACE_MIN_PID 5

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint32_t next_kstack);
extern void user_trap_return(void);

#define TRAP_CTX_SYSCALL_NUM(ctx) ((ctx)->x[17])
#define TRAP_CTX_ARG0(ctx) ((ctx)->x[10])
#define TRAP_CTX_ARG1(ctx) ((ctx)->x[11])
#define TRAP_CTX_ARG2(ctx) ((ctx)->x[12])
#define TRAP_CTX_ARG3(ctx) ((ctx)->x[13])
#define TRAP_CTX_ARG4(ctx) ((ctx)->x[14])
#define TRAP_CTX_ARG5(ctx) ((ctx)->x[15])
#define TRAP_CTX_RET(ctx) ((ctx)->x[10])
#define TRAP_CTX_SP(ctx) ((ctx)->x[2])
#define TRAP_CTX_RA(ctx) ((ctx)->x[1])
#define TRAP_CTX_FP(ctx) ((ctx)->x[8])
#define TRAP_CTX_TP(ctx) ((ctx)->x[4])

#define TRAP_CTX_SET_RET(ctx, v) do { (ctx)->x[10] = (uint32_t)(v); } while (0)
#define TRAP_CTX_SET_ARG0(ctx, v) do { (ctx)->x[10] = (uint32_t)(v); } while (0)
#define TRAP_CTX_SET_SP(ctx, v) do { (ctx)->x[2] = (uint32_t)(v); } while (0)
#define TRAP_CTX_SET_REG(ctx, i, v) do { (ctx)->x[i] = (uint32_t)(v); } while (0)
#define TRAP_CTX_REG(ctx, i) ((ctx)->x[i])

#define TRAP_CTX_EPC(ctx) ((ctx)->sepc)
#define TRAP_CTX_STATUS(ctx) ((ctx)->sstatus)
#define TRAP_CTX_LAST_A0(ctx) ((ctx)->last_a0)
#define TRAP_CTX_KScratch0(ctx) ((ctx)->x[0])

#define TASK_CTX_PAGE_TABLE(ctx) ((ctx)->satp)
#define TASK_CTX_STATUS(ctx) ((ctx)->sstatus)

static inline void arch_task_context_set_initial_sp(task_context_t *ctx, trap_context_t *trap, uint64_t stack_top) {
    (void)ctx;
    (void)trap;
    (void)stack_top;
}

static inline task_context_t *arch_task_context_base(void *kstack_base, uint64_t stack_top, trap_context_t *trap) {
    (void)kstack_base;
    if (trap)
        return (task_context_t *)((uintptr_t)trap - sizeof(task_context_t));
    return (task_context_t *)(uintptr_t)(stack_top - sizeof(task_context_t));
}

static inline uint64_t arch_task_kernel_status(void) {
    return SSTATUS_SIE;
}

static inline uint64_t arch_user_initial_status(void) {
    return SSTATUS_SPIE | SSTATUS_FS_CLEAN;
}

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0x08b00893U;
    tramp[1] = 0x00000073U;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0x08b00893U;
    p[1] = 0x00000073U;
}

static inline uint64_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_A;
}

static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint64_t ksp) {
    (void)ctx;
    (void)ksp;
}

static inline uint64_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx, uint64_t fallback) {
    (void)ctx;
    return fallback;
}

static inline void arch_advance_syscall_epc(trap_context_t *ctx) {
    TRAP_CTX_EPC(ctx) += 4;
}

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc, const trap_context_t *ctx) {
    for (int i = 0; i < 32; i++)
        sc->sc_regs[i] = ctx->x[i];
    sc->sc_pc = ctx->sepc;
    sc->sc_status = ctx->sstatus;
}

static inline void arch_signal_build_frame_extra(void *extra, const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx, const arch_sigcontext_t *sc) {
    for (int i = 0; i < 32; i++)
        ctx->x[i] = sc->sc_regs[i];
    ctx->sepc = sc->sc_pc;
    ctx->sstatus = sc->sc_status;
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx, const void *extra) {
    (void)ctx;
    (void)extra;
}

#endif
