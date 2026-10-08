#ifndef A20_HOST_LOCK_CORE_CPU_H
#define A20_HOST_LOCK_CORE_CPU_H
#include <stdint.h>
#include "core/consts.h"
struct backtrace_frame { uint64_t pc; };
unsigned cpu_current_id(void);
int arch_irqs_enabled(void);
void arch_local_irq_disable(void);
void arch_local_irq_enable(void);
void arch_cpu_relax(void);
int arch_unwind_frames(uint64_t frame, struct backtrace_frame *out,
                       unsigned capacity);
void kallsyms_print(uint64_t addr);
#endif
