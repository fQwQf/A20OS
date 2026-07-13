#ifndef _ARCH_X86_64_TRAP_H
#define _ARCH_X86_64_TRAP_H

#include "core/types.h"
#include "page_table.h"
#include "platform.h"

/*
 * Trap context layout for x86_64.
 * Saved by trap.S on the kernel stack.
 */
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags, cs, ss;
    uint64_t kernel_tp;
    uint64_t kscratch0;
    uint64_t padding[2];          /* 176: kernel top, 184: tp */
    uint64_t saved_kstack;        /* 192: original task->kstack (saved ctx ptr) */
    uint64_t reserved;            /* 200: pad to 16 bytes */
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 26 * 8, "TrapContext must be 208 bytes");

/*
 * Task context layout for x86_64.
 * Saved by __switch on the kernel stack.
 */
typedef struct {
    uint64_t ra;        /* return address */
    uint64_t tp;        /* task pointer */
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t cr3;
    uint64_t padding[5];  /* total = 16*8 = 128 bytes */
} task_context_t;

_Static_assert(sizeof(task_context_t) == 16 * 8, "TaskContext must be 128 bytes");

/* Signal context */
typedef struct {
    uint64_t fault_addr;
    uint64_t regs[23];
    uint64_t rflags;
    uint64_t reserved[512];
} __attribute__((aligned(16))) arch_sigcontext_t;

#define ARCH_SIGFRAME_EXTRA_FIELDS uint64_t arch_extra;

#define TRAP_CONTEXT_SIZE   (26 * 8)
#define TASK_CONTEXT_SIZE   (16 * 8)
#define KTRAP_CONTEXT_SIZE  (26 * 8)
#define ARCH_SYSCALL_TRACE_MIN_PID 3

extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);
extern void __switch(uint64_t next_kstack);
extern void user_trap_return(void);
extern void idt_flush(uint64_t idtr_ptr);

/*
 * x86_64 Linux syscall ABI:
 *   rax = syscall number
 *   rdi, rsi, rdx, r10, r8, r9 = arguments
 *   rcx, r11 clobbered by syscall/sysret
 */
#define TRAP_CTX_SYSCALL_NUM(ctx)  ((ctx)->rax)
#define TRAP_CTX_ARG0(ctx)         ((ctx)->rdi)
#define TRAP_CTX_ARG1(ctx)         ((ctx)->rsi)
#define TRAP_CTX_ARG2(ctx)         ((ctx)->rdx)
#define TRAP_CTX_ARG3(ctx)         ((ctx)->r10)
#define TRAP_CTX_ARG4(ctx)         ((ctx)->r8)
#define TRAP_CTX_ARG5(ctx)         ((ctx)->r9)
#define TRAP_CTX_RET(ctx)          ((ctx)->rax)
#define TRAP_CTX_SP(ctx)           ((ctx)->rsp)
#define TRAP_CTX_RA(ctx)           ((ctx)->rbp)  /* use rbp for backtrace */
#define TRAP_CTX_FP(ctx)           ((ctx)->rbp)
#define TRAP_CTX_TP(ctx)           ((ctx)->padding[1])

#define TRAP_CTX_SET_RET(ctx, v)   do { (ctx)->rax = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_ARG0(ctx, v)  do { (ctx)->rdi = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_SP(ctx, v)    do { (ctx)->rsp = (uint64_t)(v); } while(0)

static inline uint64_t arch_trap_ctx_reg(const trap_context_t *ctx, int i) {
    switch (i) {
        case 0: return ctx->rax;
        case 1: return ctx->rbx;
        case 2: return ctx->rcx;
        case 3: return ctx->rdx;
        case 4: return ctx->rsi;
        case 5: return ctx->rdi;
        case 6: return ctx->rbp;
        case 7: return ctx->rsp;
        case 8: return ctx->r8;
        case 9: return ctx->r9;
        case 10: return ctx->r10;
        case 11: return ctx->r11;
        case 12: return ctx->r12;
        case 13: return ctx->r13;
        case 14: return ctx->r14;
        case 15: return ctx->r15;
        case 16: return ctx->rip;
        default: return 0;
    }
}

static inline void arch_trap_ctx_set_reg(trap_context_t *ctx, int i, uint64_t v) {
    switch (i) {
        case 0: ctx->rax = v; break;
        case 1: ctx->rbx = v; break;
        case 2: ctx->rcx = v; break;
        case 3: ctx->rdx = v; break;
        case 4: ctx->rsi = v; break;
        case 5: ctx->rdi = v; break;
        case 6: ctx->rbp = v; break;
        case 7: ctx->rsp = v; break;
        case 8: ctx->r8  = v; break;
        case 9: ctx->r9  = v; break;
        case 10: ctx->r10 = v; break;
        case 11: ctx->r11 = v; break;
        case 12: ctx->r12 = v; break;
        case 13: ctx->r13 = v; break;
        case 14: ctx->r14 = v; break;
        case 15: ctx->r15 = v; break;
        case 16: ctx->rip = v; break;
    }
}

#define TRAP_CTX_REG(ctx, i)       arch_trap_ctx_reg((ctx), (i))
#define TRAP_CTX_SET_REG(ctx, i, v) arch_trap_ctx_set_reg((ctx), (i), (uint64_t)(v))

#define TRAP_CTX_EPC(ctx)          ((ctx)->rip)
#define TRAP_CTX_STATUS(ctx)       ((ctx)->rflags)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->kscratch0)

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->cr3)
#define TASK_CTX_STATUS(ctx)       ((ctx)->rflags)
#define TASK_CTX_SET_SP(ctx, v)    do { (ctx)->rsp = (uint64_t)(v); } while (0)

static inline void arch_task_context_set_initial_sp(task_context_t *ctx,
                                                      trap_context_t *trap,
                                                      uint64_t stack_top) {
    /* __switch loads rsp directly from the saved context.  For user tasks the
     * resume path must land on the trap_context_t; for kernel threads it must
     * land at the top of the kernel stack. */
    ctx->rsp = trap ? (uint64_t)trap : stack_top;
    /* syscall_entry needs the real kernel stack top independent of whether
     * the resume target is the pre-allocated trap frame (user) or ks_top
     * (kernel thread).  Keep it in an unused padding slot; use a volatile
     * store so the compiler cannot dead-store-eliminate the write. */
    *(volatile uint64_t *)&ctx->padding[0] = stack_top;
}

static inline task_context_t *arch_task_context_base(void *kstack_base,
                                                      uint64_t stack_top,
                                                      trap_context_t *trap) {
    (void)stack_top;
    (void)trap;
    /* On x86_64 the task_context_t lives at the bottom of the kernel stack so
     * the trap frame at the top and the C call stack in between do not
     * overwrite it.  __switch resumes using ctx->rsp, not the context base. */
    return (task_context_t *)kstack_base;
}

static inline uint64_t arch_task_kernel_status(void) {
    return SSTATUS_SIE;
}

static inline uint64_t arch_user_initial_status(void) {
    return SSTATUS_SPIE | SSTATUS_FS_CLEAN;
}

static inline void arch_trap_ctx_set_user_entry(trap_context_t *ctx,
                                                uint64_t entry) {
    TRAP_CTX_EPC(ctx) = entry;
}

static inline void arch_trap_ctx_set_kernel_stack(trap_context_t *ctx, uint64_t ksp) {
    ctx->padding[0] = ksp;
    if (ctx->cs == 0)
        ctx->cs = 0x1B;
    if (ctx->ss == 0)
        ctx->ss = 0x23;
}

static inline uint64_t arch_trap_ctx_get_kernel_stack(const trap_context_t *ctx, uint64_t fallback) {
    return ctx->padding[0] ? ctx->padding[0] : fallback;
}

static inline void arch_advance_syscall_epc(trap_context_t *ctx) {
    /*
     * Unlike ecall/syscall-style traps on other architectures, x86 interrupt
     * gates save RIP after the int instruction.  Restart paths still use
     * ARCH_SYSCALL_INSN_SIZE to rewind to the int $0x80.
     */
    (void)ctx;
}

#define ARCH_SYSCALL_INSN_SIZE 2

/* Signal trampoline: mov $15,%eax; syscall */
static inline void arch_signal_prepare_trampoline(uint32_t tramp[2]) {
    uint8_t *p = (uint8_t *)tramp;
    p[0] = 0xB8;         /* mov imm32,%eax */
    p[1] = 15;           /* rt_sigreturn */
    p[2] = 0;
    p[3] = 0;
    p[4] = 0;
    p[5] = 0x0F;         /* syscall */
    p[6] = 0x05;
    p[7] = 0;
}

static inline void arch_signal_write_trampoline(void *page) {
    uint8_t *p = (uint8_t *)page;
    p[0] = 0xB8;         /* mov imm32,%eax */
    p[1] = 15;           /* rt_sigreturn */
    p[2] = 0;
    p[3] = 0;
    p[4] = 0;
    p[5] = 0x0F;         /* syscall */
    p[6] = 0x05;
    p[7] = 0;
}

static inline uint64_t arch_signal_tramp_pte_flags(void) {
    return PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D;
}

static inline void arch_signal_build_mcontext(arch_sigcontext_t *sc,
                                              const trap_context_t *ctx) {
    sc->fault_addr = 0;
    sc->regs[0]  = ctx->rax;
    sc->regs[1]  = ctx->rbx;
    sc->regs[2]  = ctx->rcx;
    sc->regs[3]  = ctx->rdx;
    sc->regs[4]  = ctx->rsi;
    sc->regs[5]  = ctx->rdi;
    sc->regs[6]  = ctx->rbp;
    sc->regs[7]  = ctx->rsp;
    sc->regs[8]  = ctx->r8;
    sc->regs[9]  = ctx->r9;
    sc->regs[10] = ctx->r10;
    sc->regs[11] = ctx->r11;
    sc->regs[12] = ctx->r12;
    sc->regs[13] = ctx->r13;
    sc->regs[14] = ctx->r14;
    sc->regs[15] = ctx->r15;
    sc->regs[16] = ctx->rip;
    sc->regs[17] = ctx->rflags;
    sc->regs[18] = ctx->cs;
    sc->regs[19] = ctx->ss;
    sc->rflags = ctx->rflags;
}

static inline void arch_signal_build_frame_extra(void *extra,
                                                 const trap_context_t *ctx) {
    (void)extra;
    (void)ctx;
}

static inline void arch_signal_restore_mcontext(trap_context_t *ctx,
                                                const arch_sigcontext_t *sc) {
    ctx->rax = sc->regs[0];
    ctx->rbx = sc->regs[1];
    ctx->rcx = sc->regs[2];
    ctx->rdx = sc->regs[3];
    ctx->rsi = sc->regs[4];
    ctx->rdi = sc->regs[5];
    ctx->rbp = sc->regs[6];
    ctx->rsp = sc->regs[7];
    ctx->r8  = sc->regs[8];
    ctx->r9  = sc->regs[9];
    ctx->r10 = sc->regs[10];
    ctx->r11 = sc->regs[11];
    ctx->r12 = sc->regs[12];
    ctx->r13 = sc->regs[13];
    ctx->r14 = sc->regs[14];
    ctx->r15 = sc->regs[15];
    ctx->rip = sc->regs[16];
    ctx->rflags = sc->rflags;
}

static inline void arch_signal_restore_frame_extra(trap_context_t *ctx,
                                                   const void *extra) {
    (void)ctx;
    (void)extra;
}

#endif
