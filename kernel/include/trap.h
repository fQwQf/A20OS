#ifndef _TRAP_H
#define _TRAP_H

#include "types.h"
#include "consts.h"

/*
 * TrapContext - saved on kernel stack when trap occurs from user mode.
 * Layout MUST match trap.S exactly:
 *   offset 0-248:   x[0..31]  general registers (x[2]=sp, x[4]=tp)
 *   offset 256:     sstatus   (32*8)
 *   offset 264:     sepc      (33*8)
 *   offset 272:     kernel_tp (34*8)
 *   [padding to 16-byte alignment]
 *   Total: 288 bytes (36*8), aligned 16
 */
typedef struct {
    uint64_t x[32];
    uint64_t sstatus;
    uint64_t sepc;
    uint64_t kernel_tp;
} __attribute__((aligned(16))) trap_context_t;

_Static_assert(sizeof(trap_context_t) == 36 * 8, "TrapContext must be 288 bytes");

/*
 * TaskContext - saved on kernel stack during __switch
 * Layout MUST match switch.S exactly:
 *   offset 0:   ra
 *   offset 8:   tp
 *   offset 16:  s[0]  (at (0+2)*8 = 16)
 *   offset 24:  s[1]  (at (1+2)*8 = 24)
 *   ...
 *   offset 104: s[11] (at (11+2)*8 = 104)
 *   offset 112: satp   (at 14*8)
 *   offset 120: padding
 *   Total: 16*8 = 128 bytes
 */
typedef struct {
    uint64_t ra;
    uint64_t tp;
    uint64_t s[12];
    uint64_t satp;
    uint64_t sstatus;
} task_context_t;

_Static_assert(sizeof(task_context_t) == 16 * 8, "TaskContext must be 128 bytes");

/* Assembly entry points (defined in trap.S) */
extern void __trap_from_user(void);
extern void __return_to_user(void);
extern void __trap_from_kernel(void);

/* Context switch (defined in switch.S) */
extern void __switch(uint64_t next_kstack);

struct task_t;

/* C handlers called from assembly */
void trap_handler(trap_context_t *ctx);
void kernel_trap_handler(trap_context_t *ctx);

int handle_cow_fault(struct task_t *t, uint64_t stval);
int handle_demand_fault(struct task_t *t, uint64_t stval);

/* Initialization */
void trap_init(void);

#endif /* _TRAP_H */
