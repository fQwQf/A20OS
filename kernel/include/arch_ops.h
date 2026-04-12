#ifndef _ARCH_OPS_H
#define _ARCH_OPS_H

#include "types.h"

/*
 * Trap cause semantic constants consumed by generic C trap logic.
 * Architecture backends should normalize native cause encoding to these.
 */
#define ARCH_TRAP_INTERRUPT_MASK  (1UL << 63)
#define ARCH_TRAP_CODE_MASK       0xFFUL
#define ARCH_IRQ_CAUSE_SOFT       1UL
#define ARCH_IRQ_CAUSE_TIMER      5UL
#define ARCH_IRQ_CAUSE_EXTERNAL   9UL
#define ARCH_TRAP_ECALL_FROM_U    8UL

/* Save current IRQ state and disable IRQ delivery. Returns restorable state. */
uint64_t arch_irq_save(void);
/* Restore IRQ state returned by arch_irq_save(). */
void arch_irq_restore(uint64_t state);
/* Globally enable IRQ delivery for current CPU. */
void arch_irq_enable(void);
/* Globally disable IRQ delivery for current CPU. */
void arch_irq_disable(void);
/* Enable timer interrupt source. */
void arch_irq_enable_timer(void);
/* Disable timer interrupt source. */
void arch_irq_disable_timer(void);
/* Enable external interrupt source(s). */
void arch_irq_enable_external(void);
/* Clear pending software interrupt for current CPU. */
void arch_softirq_clear(void);

/* Set trap/exception entry vector address. */
void arch_trap_vector_set(uint64_t addr);
/* Set trap scratch register/state used by low-level entry code. */
void arch_trap_scratch_set(uint64_t val);
/* Read normalized trap cause (uses ARCH_TRAP_* semantics). */
uint64_t arch_trap_cause(void);
/* Read exception program counter for current trap. */
uint64_t arch_trap_epc(void);
/* Write exception program counter for trap return. */
void arch_trap_set_epc(uint64_t epc);
/* Read trap value register (fault address or arch-defined extra info). */
uint64_t arch_trap_tval(void);

/* Build architecture MMU context token from page table root pointer. */
uint64_t arch_mmu_token_from_pgdir(uint64_t *pgdir);
/* Return default task status bits for entering kernel task context. */
uint64_t arch_task_status_kernel_default(void);
/* Set current task pointer register/state used by fast current-task lookup. */
void arch_set_current_task_ptr(void *task);

/* Synchronize instruction cache after code/data updates. */
void arch_sync_icache(void);

/* Enter low-power wait state until next event/interrupt. */
void arch_cpu_wait(void);
/* Short CPU relax hint for spin/busy loops. */
void arch_cpu_relax(void);

/* Full memory barrier. */
void arch_mb(void);
/* Read memory barrier. */
void arch_rmb(void);
/* Write memory barrier. */
void arch_wmb(void);

/* Read architecture timer/counter ticks. */
uint64_t arch_timer_counter(void);
/* Program timer to a target tick value (backend-defined semantics). */
void arch_timer_program(uint64_t deadline);

/* Initialize UART hardware backend used by drv/uart.c. */
void arch_uart_init_hw(void);
/* Write one byte to UART backend (polling). */
void arch_uart_putc_hw(char c);
/* Try reading one byte from UART backend; return -1 if no data. */
int  arch_uart_try_getc_hw(void);

#endif /* _ARCH_OPS_H */
