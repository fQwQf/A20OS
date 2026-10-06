#ifndef _CORE_PREEMPT_H
#define _CORE_PREEMPT_H

#include "core/types.h"
#include "core/consts.h"
/* CONFIG_NR_CPUS and cpu_current_id() both live in core/cpu.h, not in
 * core/consts.h; the consts.h include is kept for the width/limit definitions
 * preempt.h's callers already rely on that header for. */
#include "core/cpu.h"

#ifdef CONFIG_KERNEL_PREEMPT

/*
 * Per-CPU preemption state.
 *
 * preempt is a nesting depth, not a boolean: spin_lock_at() takes it, so the
 * counter is what makes "no context switch while a lock is held" hold for the
 * whole critical section rather than only for its first instruction.
 * hardirq is the separate flag for "an interrupt handler is running on this
 * CPU" -- it is what keeps a task that was interrupted mid-lock from being
 * considered preemptible just because it left the lock.
 *
 * The line is padded to a cache line because these are read and written on
 * every IRQ return; sharing a line with another CPU's slot would turn a
 * per-CPU counter into a coherence storm.
 */
typedef struct __attribute__((aligned(64))) cpu_preempt_state {
    unsigned preempt;  /* >0: spinlock section / explicit no-preempt nesting */
    unsigned hardirq;  /* nonzero: a hardware interrupt handler is running */
} cpu_preempt_state_t;

void preempt_state_init(unsigned cpu);
void preempt_disable(void);
void preempt_enable(void);
unsigned preempt_count(void);
/* Only a task outside every lock, and outside hardirq, may be switched out
 * from the IRQ return path. */
int preempt_allowed(void);
void hardirq_enter(void);
void hardirq_exit(void);
int in_hardirq(void);

#else /* !CONFIG_KERNEL_PREEMPT */

/*
 * CONFIG_KERNEL_PREEMPT off: the cooperative model.  Everything compiles
 * unchanged and every predicate is a constant, so the call sites fold away
 * and a non-preempting build pays nothing for the hook.
 */
static inline void preempt_state_init(unsigned cpu) { (void)cpu; }
static inline void preempt_disable(void) { }
static inline void preempt_enable(void) { }
static inline unsigned preempt_count(void) { return 0; }
static inline int preempt_allowed(void) { return 0; }
static inline void hardirq_enter(void) { }
static inline void hardirq_exit(void) { }
static inline int in_hardirq(void) { return 0; }

#endif /* CONFIG_KERNEL_PREEMPT */

#endif /* _CORE_PREEMPT_H */