#ifndef _ARCH_LOONGARCH64_TRAP_H
#define _ARCH_LOONGARCH64_TRAP_H

#include "types.h"

typedef struct {
    uint64_t regs[32];
    uint64_t era;
    uint64_t prmd;
    uint64_t kernel_tp;
    uint64_t kernel_sp;
} __attribute__((aligned(16))) trap_context_t;

/* 35 fields * 8 = 280, aligned(16) pads to 288 */
_Static_assert(sizeof(trap_context_t) == 36 * 8, "TrapContext must be 288 bytes");

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

#define TRAP_CONTEXT_SIZE  (36 * 8)
#define TASK_CONTEXT_SIZE  (15 * 8)
#define KTRAP_CONTEXT_SIZE (34 * 8)

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

#define TRAP_CTX_SET_RET(ctx, v)  do { (ctx)->regs[4] = (uint64_t)(v); } while(0)
#define TRAP_CTX_SET_SP(ctx, v)   do { (ctx)->regs[3] = (uint64_t)(v); } while(0)

#define TRAP_CTX_EPC(ctx)          ((ctx)->era)
#define TRAP_CTX_STATUS(ctx)         ((ctx)->prmd)
#define TRAP_CTX_KScratch0(ctx)    ((ctx)->regs[0])

#define TASK_CTX_PAGE_TABLE(ctx)   ((ctx)->pgdl)
#define TASK_CTX_STATUS(ctx)       ((ctx)->prmd)

#endif
