#ifdef CONFIG_ARMV7M

#include "core/types.h"
#include "shared_kernel.h"
#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "program_stack.h"
uint64_t armv7m_exception_stack[ARMV7M_EXCEPTION_STACK_BYTES /
                                sizeof(uint64_t)] MCU_PRIVATE_BSS;
uint32_t armv7m_program_user_ticks MCU_PRIVATE_BSS;
#endif

volatile uint32_t armv7m_trap_cause MCU_PRIVATE_BSS;
volatile uint32_t armv7m_fault_addr MCU_PRIVATE_BSS;
volatile uint32_t armv7m_fault_pc MCU_PRIVATE_BSS;
void *armv7m_task_pointer MCU_PRIVATE_BSS;
/* Cortex-M task context switching has no separate privileged TLS register. */
void arch_switch_tls_restore(void) {}
#ifdef CONFIG_MCU_SHARED_KERNEL
unsigned armv7m_private_cpu_id MCU_PRIVATE_BSS;
unsigned armv7m_private_shared_ready MCU_PRIVATE_BSS;
#endif

#endif
