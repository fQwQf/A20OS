#ifndef _ARCH_LOONGARCH64_TRAP_H
#define _ARCH_LOONGARCH64_TRAP_H

#include "core/types.h"
#include "page_table.h"

typedef struct {
    uint64_t regs[32];
    uint64_t era;
    uint64_t prmd;
    uint64_t kernel_tp;
    uint64_t kernel_sp;
    uint64_t f[32];
    uint64_t fcsr;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 70 * 8, "TrapContext must be 560 bytes");

typedef struct {
    uint64_t ra;
    uint64_t tp;
    uint64_t s[9];
    uint64_t fp;
    uint64_t sp;
    uint64_t pgdl;
    uint64_t prmd;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 15 * 8, "TaskContext must be 120 bytes");

typedef struct {
    uint64_t sc_pc;
    uint64_t sc_regs[32];
    uint32_t sc_flags;
    uint64_t sc_extcontext[0] __attribute__((aligned(16)));
} __attribute__((aligned(16))) arch_sigcontext_t;

#define ARCH_UCONTEXT_PAD_FIELDS uint64_t uc_pad;

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint64_t padding;
} __attribute__((aligned(16))) arch_loongarch_sctx_info_t;

typedef struct {
    uint64_t regs[32];
    uint64_t fcc;
    uint32_t fcsr;
    uint32_t reserved;
} __attribute__((aligned(8))) arch_loongarch_fpu_context_t;

typedef struct {
    arch_loongarch_sctx_info_t fpu_head;
    arch_loongarch_fpu_context_t fpu;
    arch_loongarch_sctx_info_t end;
} __attribute__((aligned(16))) arch_sigframe_extra_t;

#define ARCH_SIGFRAME_EXTRA_FIELDS arch_sigframe_extra_t arch_extra;

#define TRAP_CONTEXT_SIZE  (70 * 8)
#define TASK_CONTEXT_SIZE  (15 * 8)
#define KTRAP_CONTEXT_SIZE (70 * 8)
#define ARCH_SYSCALL_TRACE_MIN_PID 3

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint64_t next_kstack);
extern void user_trap_return(void);
extern void trap_handler_la64(trap_context_t *ctx);

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

#define TRAP_CTX_SET_RET(ctx, v)  do { (ctx)->regs[4] = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_ARG0(ctx, v) do { (ctx)->regs[4] = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_SP(ctx, v)   do { (ctx)->regs[3] = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_REG(ctx, i, v) do { (ctx)->regs[i] = (uint64_t)(v); } while(0)
#define TRAP_CTX_REG(ctx, i)      ((ctx)->regs[i])

#define TRAP_CTX_EPC(ctx)          ((ctx)->era)
#define TRAP_CTX_STATUS(ctx)         ((ctx)->prmd)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->regs[0])

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->pgdl)
#define TASK_CTX_STATUS(ctx)       ((ctx)->prmd)

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0x02822c0b;
    tramp[1] = 0x002b0000;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0x02822c0b;
    p[1] = 0x002b0000;
}

static inline uint64_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_D | PTE_MAT1 | PTE_LEAF;
}

static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint64_t ksp) {
    ctx->kernel_sp = ksp;
}

static inline uint64_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx, uint64_t fallback) {
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

static inline void arch_signal_build_frame_extra(arch_sigframe_extra_t *extra,
                                                 const trap_context_t *ctx) {
    extra->fpu_head.magic = 0x46505501U;
    extra->fpu_head.size = sizeof(extra->fpu_head) + sizeof(extra->fpu);
    extra->fpu_head.padding = 0;
    for (int i = 0; i < 32; i++)
        extra->fpu.regs[i] = ctx->f[i];
    extra->fpu.fcc = 0;
    extra->fpu.fcsr = (uint32_t)ctx->fcsr;
    extra->fpu.reserved = 0;
    extra->end.magic = 0;
    extra->end.size = 0;
    extra->end.padding = 0;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx,
                                                const arch_sigcontext_t *sc) {
    for (int i = 0; i < 32; i++)
        ctx->regs[i] = sc->sc_regs[i];
    ctx->era = sc->sc_pc;
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx,
                                                   const arch_sigframe_extra_t *extra) {
    if (extra->fpu_head.magic != 0x46505501U ||
        extra->fpu_head.size < sizeof(extra->fpu_head) + sizeof(extra->fpu))
        return;
    for (int i = 0; i < 32; i++)
        ctx->f[i] = extra->fpu.regs[i];
    ctx->fcsr = extra->fpu.fcsr;
}

#endif
