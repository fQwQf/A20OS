#ifndef A20_ARMV7M_SHARED_KERNEL_H
#define A20_ARMV7M_SHARED_KERNEL_H

#ifdef CONFIG_MCU_SHARED_KERNEL
#include "core/types.h"
#include "core/smp.h"
#include "core/shared_fabric.h"

#define MCU_PRIVATE_BSS __attribute__((section(".private.bss")))
#define MCU_PRIVATE_DATA __attribute__((section(".private.data")))
/* A20 fabric ABI v1, not registers of an off-the-shelf STM32 peripheral. */
#define MCU_FABRIC_BASE 0x64000000UL

extern unsigned armv7m_private_shared_ready;
uint32_t mcu_fabric_lock(unsigned lock);
void mcu_fabric_unlock(unsigned lock, uint32_t flags);
void mcu_fabric_poll_ipi(void);
void mcu_fabric_bus_init(void);
void armv7m_shared_reset(void) __attribute__((noreturn));
extern const smp_platform_ops_t mcu_fabric_smp_ops;
#ifdef CONFIG_MCU_SHARED_TEST
void mcu_shared_selftest_start(void);
#endif
#else
#define MCU_PRIVATE_BSS
#define MCU_PRIVATE_DATA
#endif
#endif
