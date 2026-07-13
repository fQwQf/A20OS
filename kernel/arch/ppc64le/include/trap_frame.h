#ifndef _ARCH_PPC64LE_TRAP_H
#define _ARCH_PPC64LE_TRAP_H

#include "core/types.h"
#include "page_table.h"
#include "platform.h"
#include "asm/ppc64-regs.h"

typedef struct {
    uint64_t gpr[32];
    uint64_t nip;
    uint64_t msr;
    uint64_t ctr;
    uint64_t ra;
    uint64_t xer;
    uint64_t cr;
    uint64_t kernel_tp;
    uint64_t kernel_sp;
    uint64_t addr_space;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 42 * 8, "TrapContext must be 336 bytes");

typedef struct {
    uint64_t ra;
    uint64_t toc;
    uint64_t tp;
    uint64_t r14;
    uint64_t r15;
    uint64_t r16;
    uint64_t r17;
    uint64_t r18;
    uint64_t r19;
    uint64_t r20;
    uint64_t r21;
    uint64_t r22;
    uint64_t r23;
    uint64_t r24;
    uint64_t r25;
    uint64_t r26;
    uint64_t r27;
    uint64_t r28;
    uint64_t r29;
    uint64_t r30;
    uint64_t r31;
    uint64_t sp;
    uint64_t pgdir;
    uint64_t msr;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 24 * 8, "TaskContext must be 192 bytes");

typedef struct {
    uint64_t gp_regs[32];
    uint64_t nip;
    uint64_t msr;
    uint64_t reserved[32];
} __attribute__((aligned(16))) arch_sigcontext_t;

#define ARCH_UCONTEXT_PAD_FIELDS uint64_t uc_pad;
#define ARCH_SIGFRAME_EXTRA_FIELDS uint64_t arch_extra;

#define TRAP_CONTEXT_SIZE  (42 * 8)
#define TASK_CONTEXT_SIZE  (24 * 8)
#define KTRAP_CONTEXT_SIZE (42 * 8)
#define ARCH_SYSCALL_TRACE_MIN_PID 3
/*
 * The pseries low-vector bridge currently has one per-CPU scratch frame.
 * Keep syscall handling non-preemptible until kernel-mode nested traps have
 * their own save area.
 */
#define ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE 1

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint64_t next_kstack);
extern void user_trap_return(void);

#define TRAP_CTX_SYSCALL_NUM(ctx)  ((ctx)->gpr[0])
#define TRAP_CTX_ARG0(ctx)         ((ctx)->gpr[3])
#define TRAP_CTX_ARG1(ctx)         ((ctx)->gpr[4])
#define TRAP_CTX_ARG2(ctx)         ((ctx)->gpr[5])
#define TRAP_CTX_ARG3(ctx)         ((ctx)->gpr[6])
#define TRAP_CTX_ARG4(ctx)         ((ctx)->gpr[7])
#define TRAP_CTX_ARG5(ctx)         ((ctx)->gpr[8])
#define TRAP_CTX_RET(ctx)          ((ctx)->gpr[3])
#define TRAP_CTX_SP(ctx)           ((ctx)->gpr[1])
#define TRAP_CTX_RA(ctx)           ((ctx)->ra)
#define TRAP_CTX_FP(ctx)           ((ctx)->gpr[31])
#define TRAP_CTX_TP(ctx)           ((ctx)->gpr[13])

#define TRAP_CTX_SET_RET(ctx, v)   do { (ctx)->gpr[3] = (uint64_t)(v); } while (0)
#define TRAP_CTX_SET_ARG0(ctx, v)  do { (ctx)->gpr[3] = (uint64_t)(v); } while (0)
#define TRAP_CTX_SET_SP(ctx, v)    do { (ctx)->gpr[1] = (uint64_t)(v); } while (0)
#define TRAP_CTX_SET_REG(ctx, i, v) do { (ctx)->gpr[i] = (uint64_t)(v); } while (0)
#define TRAP_CTX_REG(ctx, i)       ((ctx)->gpr[i])

#define TRAP_CTX_EPC(ctx)          ((ctx)->nip)
#define TRAP_CTX_STATUS(ctx)       ((ctx)->msr)
#define TRAP_CTX_LAST_A0(ctx)      ((ctx)->gpr[3])
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->addr_space)

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->pgdir)
#define TASK_CTX_STATUS(ctx)       ((ctx)->msr)

static inline void arch_task_context_set_initial_sp(task_context_t *ctx,
                                                     trap_context_t *trap,
                                                     uint64_t stack_top) {
    uint64_t toc;
    __asm__ __volatile__("mr %0,2" : "=r"(toc));
    ctx->toc = toc;
    ctx->sp = trap ? (uint64_t)trap : stack_top;
}

static inline task_context_t *arch_task_context_base(void *kstack_base,
                                                      uint64_t stack_top,
                                                      trap_context_t *trap) {
    (void)stack_top;
    (void)trap;
    return (task_context_t *)kstack_base;
}

static inline uint64_t arch_task_kernel_status(void) {
    return PPC64_MSR_SF | PPC64_MSR_IR |
           PPC64_MSR_DR | PPC64_MSR_RI | PPC64_MSR_LE;
}

static inline uint64_t arch_user_initial_status(void) {
    return PPC64_MSR_SF | PPC64_MSR_ISF | PPC64_MSR_PR |
           PPC64_MSR_EE | PPC64_MSR_FP |
           PPC64_MSR_IR | PPC64_MSR_DR | PPC64_MSR_LE;
}

static inline void arch_trap_ctx_set_user_entry(trap_context_t *ctx,
                                                uint64_t entry) {
    ctx->nip = entry;
    ctx->gpr[12] = entry;
}

static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    tramp[0] = 0x380000acU;
    tramp[1] = 0x44000002U;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint32_t *p = (uint32_t *)page;
    p[0] = 0x380000acU;
    p[1] = 0x44000002U;
}

static inline uint64_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF;
}

static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint64_t ksp) {
    ctx->kernel_sp = ksp;
}

static inline uint64_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx, uint64_t fallback) {
    return ctx->kernel_sp ? ctx->kernel_sp : fallback;
}

static inline void arch_advance_syscall_epc(trap_context_t *ctx) {
    (void)ctx;
}

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc,
                                              const trap_context_t *ctx) {
    for (int i = 0; i < 32; i++)
        sc->gp_regs[i] = ctx->gpr[i];
    sc->nip = ctx->nip;
    sc->msr = ctx->msr;
}

static inline void arch_signal_build_frame_extra(void *extra,
                                                 const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx,
                                                const arch_sigcontext_t *sc) {
    for (int i = 0; i < 32; i++)
        ctx->gpr[i] = sc->gp_regs[i];
    ctx->nip = sc->nip;
    ctx->msr = sc->msr;
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx,
                                                   const void *extra) {
    (void)ctx;
    (void)extra;
}

#endif
