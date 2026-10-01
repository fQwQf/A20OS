#ifndef _DRIVERS_IRQCHIP_PLIC_H
#define _DRIVERS_IRQCHIP_PLIC_H

#include <stdint.h>

#include "drivers/core/driver_core.h"

/*
 * RISC-V Platform-Level Interrupt Controller.
 *
 * Every RISC-V SoC in this tree presents the same PLIC: QEMU virt, StarFive
 * JH7110 (VisionFive 2), SophGo SG2000 (LicheeRV Nano) and CV1800B (Milk-V
 * Duo), Kendryte K230, and Allwinner D1/F133.  The controllers differ only in
 * the MMIO base and in how a hart id is obtained, so the controller body is
 * shared here instead of being re-derived per board.
 *
 * Boards call plic_configure() from their early_init() and then set
 * board_config_t::irqchip to &plic_irqchip_ops.
 */

/* 32-bit words in one hart's enable block; each covers 32 interrupts, so
 * interrupt N is bit N%32 of word N/32.  Exposed because the arch trap path
 * programs the same register. */
#define PLIC_ENABLE_WORDS 32U

/* Returns the hardware hart id of the calling hart.  Supplied by the board
 * because riscv64 exposes arch_cpu_hart_id() while riscv32 does not. */
typedef uint64_t (*plic_hart_id_fn)(void);

/* Publish the controller's MMIO base and the hart-id resolver.  Must be called
 * before any irqchip op runs. */
void plic_configure(uintptr_t base, plic_hart_id_fn hart_id);

/* The shared controller.  A const object rather than a function so that a
 * board's static board_config_t can point at it directly. */
extern const irqchip_ops_t plic_irqchip_ops;

/* Register block accessors, for arch trap paths that claim interrupts
 * directly rather than through the irqchip_ops_t::ack callback. */
uintptr_t plic_base(void);
uint32_t plic_claim(void);
void     plic_complete(uint32_t irq);
uint32_t plic_pending(void);

#endif /* _DRIVERS_IRQCHIP_PLIC_H */
