#ifndef _ARCH_RISCV64_TRAP_H
#define _ARCH_RISCV64_TRAP_H

#include "core/types.h"

typedef struct {
    uint64_t x[32];
    uint64_t sstatus;
    uint64_t sepc;
    uint64_t last_a0;
    uint64_t kernel_tp;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 36 * 8, "TrapContext must be 288 bytes");

typedef struct {
    uint64_t ra;
    uint64_t tp;
    uint64_t s[12];
    uint64_t satp;
    uint64_t sstatus;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 16 * 8, "TaskContext must be 128 bytes");

#define TRAP_CONTEXT_SIZE  (36 * 8)
#define TASK_CONTEXT_SIZE  (16 * 8)
#define KTRAP_CONTEXT_SIZE (34 * 8)
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

#endif
