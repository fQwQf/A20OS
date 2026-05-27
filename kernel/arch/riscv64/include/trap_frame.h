#ifndef _ARCH_RISCV64_TRAP_H
#define _ARCH_RISCV64_TRAP_H

#include "core/types.h"
#include "page_table.h"

typedef struct {
    uint64_t x[32];
    uint64_t sstatus;
    uint64_t sepc;
    uint64_t last_a0;
    uint64_t kernel_tp;
    uint64_t f[32];
    uint64_t fcsr;
    uint64_t reserved;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 70 * 8, "TrapContext must be 560 bytes");

typedef struct {
    uint64_t ra;
    uint64_t tp;
    uint64_t s[12];
    uint64_t satp;
    uint64_t sstatus;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 16 * 8, "TaskContext must be 128 bytes");

typedef struct {
    uint64_t sc_regs[32];
    uint64_t sc_fpregs[66];
} __attribute__((aligned(16))) arch_sigcontext_t;

#define ARCH_SIGFRAME_EXTRA_FIELDS uint64_t arch_extra;

#define TRAP_CONTEXT_SIZE  (70 * 8)
#define TASK_CONTEXT_SIZE  (16 * 8)
#define KTRAP_CONTEXT_SIZE (70 * 8)
#define ARCH_SYSCALL_TRACE_MIN_PID 5

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint64_t next_kstack);
extern void user_trap_return(void);

#define TRAP_CTX_SYSCALL_NUM(ctx)  ((ctx)->x[17])
#define TRAP_CTX_ARG0(ctx)        ((ctx)->x[10])
#define TRAP_CTX_ARG1(ctx)        ((ctx)->x[11])
#define TRAP_CTX_ARG2(ctx)        ((ctx)->x[12])
#define TRAP_CTX_ARG3(ctx)        ((ctx)->x[13])
#define TRAP_CTX_ARG4(ctx)        ((ctx)->x[14])
#define TRAP_CTX_ARG5(ctx)        ((ctx)->x[15])
#define TRAP_CTX_RET(ctx)         ((ctx)->x[10])
#define TRAP_CTX_SP(ctx)          ((ctx)->x[2])
#define TRAP_CTX_RA(ctx)          ((ctx)->x[1])
#define TRAP_CTX_FP(ctx)          ((ctx)->x[8])
#define TRAP_CTX_TP(ctx)          ((ctx)->x[4])

#define TRAP_CTX_SET_RET(ctx, v)  do { (ctx)->x[10] = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_ARG0(ctx, v) do { (ctx)->x[10] = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_SP(ctx, v)   do { (ctx)->x[2] = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_REG(ctx, i, v) do { (ctx)->x[i] = (uint64_t)(v); } while(0)
#define TRAP_CTX_REG(ctx, i)      ((ctx)->x[i])

#define TRAP_CTX_EPC(ctx)          ((ctx)->sepc)
#define TRAP_CTX_STATUS(ctx)         ((ctx)->sstatus)
#define TRAP_CTX_LAST_A0(ctx)      ((ctx)->last_a0)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->x[0])

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->satp)
#define TASK_CTX_STATUS(ctx)       ((ctx)->sstatus)

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0x08b00893;
    tramp[1] = 0x00000073;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0x08b00893;
    p[1] = 0x00000073;
}

static inline uint64_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_MAT1 | PTE_LEAF;
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

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc,
                                              const trap_context_t *ctx) {
    sc->sc_regs[0] = ctx->sepc;
    for (int i = 1; i < 32; i++)
        sc->sc_regs[i] = ctx->x[i];
    for (int i = 0; i < 32; i++)
        sc->sc_fpregs[i] = ctx->f[i];
    sc->sc_fpregs[32] = ctx->fcsr;
    for (int i = 33; i < 66; i++)
        sc->sc_fpregs[i] = 0;
}

static inline void arch_signal_build_frame_extra(void *extra,
                                                 const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx,
                                                const arch_sigcontext_t *sc) {
    ctx->sepc = sc->sc_regs[0];
    for (int i = 1; i < 32; i++)
        ctx->x[i] = sc->sc_regs[i];
    for (int i = 0; i < 32; i++)
        ctx->f[i] = sc->sc_fpregs[i];
    ctx->fcsr = sc->sc_fpregs[32];
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx,
                                                   const void *extra) {
    (void)ctx;
    (void)extra;
}

#endif
