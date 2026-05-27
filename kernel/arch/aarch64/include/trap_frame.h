#ifndef _ARCH_AARCH64_TRAP_H
#define _ARCH_AARCH64_TRAP_H

#include "core/types.h"
#include "page_table.h"

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t elr;
    uint64_t spsr;
    uint64_t tpidr;
    uint64_t ttbr0;
    uint64_t kernel_tp;
    uint64_t kernel_sp;
    uint64_t v[64];
    uint64_t fpcr;
    uint64_t fpsr;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 104 * 8, "TrapContext must be 832 bytes");

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

typedef struct {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t reserved[512];
} __attribute__((aligned(16))) arch_sigcontext_t;

typedef struct {
    uint32_t magic;
    uint32_t size;
} arch_aarch64_ctx_t;

typedef struct {
    arch_aarch64_ctx_t head;
    uint32_t fpsr;
    uint32_t fpcr;
    uint64_t vregs[64];
} __attribute__((aligned(16))) arch_aarch64_fpsimd_context_t;

typedef struct {
    arch_aarch64_fpsimd_context_t fpsimd;
    arch_aarch64_ctx_t end;
} __attribute__((aligned(16))) arch_sigframe_extra_t;

#define ARCH_SIGFRAME_EXTRA_FIELDS arch_sigframe_extra_t arch_extra;

#define TRAP_CONTEXT_SIZE  (104 * 8)
#define TASK_CONTEXT_SIZE  (16 * 8)
#define KTRAP_CONTEXT_SIZE (104 * 8)
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
#define TRAP_CTX_FP(ctx)           ((ctx)->x[29])
#define TRAP_CTX_TP(ctx)           ((ctx)->tpidr)

#define TRAP_CTX_SET_RET(ctx, v)   do { (ctx)->x[0] = (uint64_t)(v); } while (0)
#define TRAP_CTX_SET_ARG0(ctx, v)  do { (ctx)->x[0] = (uint64_t)(v); } while (0)
#define TRAP_CTX_SET_SP(ctx, v)    do { (ctx)->sp = (uint64_t)(v); } while (0)

static inline uint64_t arch_trap_ctx_reg(const trap_context_t *ctx, int i) {
    if (i >= 0 && i < 31)
        return ctx->x[i];
    return i == 31 ? ctx->sp : 0;
}

static inline void arch_trap_ctx_set_reg(trap_context_t *ctx, int i, uint64_t v) {
    if (i >= 0 && i < 31)
        ctx->x[i] = v;
    else if (i == 31)
        ctx->sp = v;
}

#define TRAP_CTX_REG(ctx, i)       arch_trap_ctx_reg((ctx), (i))
#define TRAP_CTX_SET_REG(ctx, i, v) arch_trap_ctx_set_reg((ctx), (i), (uint64_t)(v))

#define TRAP_CTX_EPC(ctx)          ((ctx)->elr)
#define TRAP_CTX_STATUS(ctx)       ((ctx)->spsr)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->ttbr0)

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->ttbr0)
#define TASK_CTX_STATUS(ctx)       ((ctx)->daif)

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0xD2801168U;
    tramp[1] = 0xD4000001U;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0xD2801168U;
    p[1] = 0xD4000001U;
}

static inline uint64_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF;
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

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc,
                                              const trap_context_t *ctx) {
    for (int i = 0; i < 31; i++)
        sc->regs[i] = ctx->x[i];
    sc->sp = ctx->sp;
    sc->pc = ctx->elr;
    sc->pstate = ctx->spsr;
    arch_sigframe_extra_t *extra = (arch_sigframe_extra_t *)sc->reserved;
    extra->fpsimd.head.magic = 0x46508001U;
    extra->fpsimd.head.size = sizeof(extra->fpsimd);
    extra->fpsimd.fpsr = (uint32_t)ctx->fpsr;
    extra->fpsimd.fpcr = (uint32_t)ctx->fpcr;
    for (int i = 0; i < 64; i++)
        extra->fpsimd.vregs[i] = ctx->v[i];
    extra->end.magic = 0;
    extra->end.size = 0;
}

static inline void arch_signal_build_frame_extra(void *extra,
                                                 const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx,
                                                const arch_sigcontext_t *sc) {
    for (int i = 0; i < 31; i++)
        ctx->x[i] = sc->regs[i];
    ctx->sp = sc->sp;
    ctx->elr = sc->pc;
    ctx->spsr = sc->pstate;
    const arch_sigframe_extra_t *extra = (const arch_sigframe_extra_t *)sc->reserved;
    if (extra->fpsimd.head.magic == 0x46508001U &&
        extra->fpsimd.head.size >= sizeof(extra->fpsimd)) {
        for (int i = 0; i < 64; i++)
            ctx->v[i] = extra->fpsimd.vregs[i];
        ctx->fpsr = extra->fpsimd.fpsr;
        ctx->fpcr = extra->fpsimd.fpcr;
    }
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx,
                                                   const void *extra) {
    (void)ctx;
    (void)extra;
}

#endif
