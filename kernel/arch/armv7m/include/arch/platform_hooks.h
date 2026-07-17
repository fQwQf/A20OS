#ifndef _ARCH_ARMV7M_PLATFORM_HOOKS_H
#define _ARCH_ARMV7M_PLATFORM_HOOKS_H

#include "core/types.h"

/* Cortex-M mechanisms call these hooks without knowing the selected MCU or
 * board. The platform owns clock discovery and external-device dispatch. */
uint32_t armv7m_platform_core_clock_hz(void);
void armv7m_platform_systick(uint64_t ticks);
void armv7m_platform_irq_dispatch(uint32_t irq);
void armv7m_platform_fault_notify(void);

#endif
