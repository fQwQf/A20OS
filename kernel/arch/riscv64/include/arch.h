#ifndef _ARCH_RISCV64_H
#define _ARCH_RISCV64_H

#define ARCH_NAME "riscv64"

#define ARCH_HAS_VDSO 1

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

uint32_t riscv64_asid_alloc(void);
void riscv64_asid_release(uint32_t asid);

#define ARCH_MM_CONTEXT_ALLOC() riscv64_asid_alloc()
#define ARCH_MM_CONTEXT_RELEASE(asid) riscv64_asid_release(asid)
#define ARCH_MM_ADDRESS_SPACE_TOKEN(pgdir, asid) \
    arch_make_satp_asid((pgdir), (asid))


/* the timebase comes from the firmware DTB: QEMU virt
 * publishes 10 MHz and StarFive JH7110 24 MHz. */
#define ARCH_TIMER_FREQ_RUNTIME 1

/* The time CSR is a hardware counter: it advances even before timer
 * interrupts are being delivered, so boot code can bound a wait in real time
 * instead of a poll count (core/smp.c). */
#define ARCH_TIMER_BOOT_MONOTONIC 1

/* the current task pointer is kept in tp. */
#define ARCH_FAST_CURRENT 1

#endif
