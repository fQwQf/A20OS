#ifdef CONFIG_ARMV7M

#include "core/types.h"

volatile uint32_t armv7m_trap_cause;
volatile uint32_t armv7m_fault_addr;
volatile uint32_t armv7m_fault_pc;
void *armv7m_task_pointer;

#endif
