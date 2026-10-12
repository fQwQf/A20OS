#ifdef CONFIG_ARMV7M

#include "core/types.h"
#include "shared_kernel.h"

volatile uint32_t armv7m_trap_cause MCU_PRIVATE_BSS;
volatile uint32_t armv7m_fault_addr MCU_PRIVATE_BSS;
volatile uint32_t armv7m_fault_pc MCU_PRIVATE_BSS;
void *armv7m_task_pointer MCU_PRIVATE_BSS;
#ifdef CONFIG_MCU_SHARED_KERNEL
unsigned armv7m_private_cpu_id MCU_PRIVATE_BSS;
unsigned armv7m_private_shared_ready MCU_PRIVATE_BSS;
#endif

#endif
