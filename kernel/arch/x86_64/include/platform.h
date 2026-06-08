#ifndef _ARCH_X86_64_PLATFORM_H
#define _ARCH_X86_64_PLATFORM_H

#include "core/types.h"

/* Physical memory layout (QEMU q35, 1GB) */
#define PHYS_MEMORY_BASE   0x00000000UL
#define PHYS_MEMORY_END    0x40000000UL
#define KERNEL_ENTRY       0x00200000UL
#define PAGE_OFFSET        0xFFFF800000000000UL
#define USER_VA_LIMIT      0x0000800000000000UL

static inline size_t arch_ram_range_count(void) {
    return 1;
}

static inline int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx != 0 || !base || !end) return -1;
    *base = PHYS_MEMORY_BASE;
    *end = PHYS_MEMORY_END;
    return 0;
}

/* MMIO base addresses (kernel virtual) */
#define LAPIC_BASE         (0xFEE00000UL + PAGE_OFFSET)
#define IOAPIC_BASE        (0xFEC00000UL + PAGE_OFFSET)
#define PCI_ECAM_BASE      (0xB0000000UL + PAGE_OFFSET)
#define PCI_MMIO_BASE      (0xC0000000UL + PAGE_OFFSET)

/* UART - COM1 uses I/O ports, not MMIO */
#define UART0_PORT         0x3F8
#define UART0_IRQ          4
#define KEYBOARD_IRQ       1

/* IRQ vectors */
#define IRQ_VECTOR_TIMER   0x20
#define IRQ_VECTOR_UART    0x24
#define IRQ_VECTOR_PCI     0x22
#define IRQ_VECTOR_KEYBOARD 0x21

/* Exception / pseudo-cause codes
 * x86_64 does not have a unified cause register like RISC-V.
 * We map CPU exceptions to pseudo-codes used by the generic trap.c.
 */
#define CAUSE_ECALL_U           0x80   /* syscall instruction */
#define CAUSE_INSN_MISALIGNED   0
#define CAUSE_INSN_FAULT        1      /* #GP or #PF on fetch */
#define CAUSE_ILLEGAL_INSN      2      /* #UD */
#define CAUSE_BREAKPOINT        3      /* #BP */
#define CAUSE_LOAD_MISALIGNED   4
#define CAUSE_LOAD_FAULT        5      /* #PF on read */
#define CAUSE_STORE_MISALIGNED  6
#define CAUSE_STORE_FAULT       7      /* #PF on write */
#define CAUSE_INSN_PAGE_FAULT   12     /* #PF (same as 13/14, code disambiguates) */
#define CAUSE_LOAD_PAGE_FAULT   13     /* #PF on read */
#define CAUSE_STORE_PAGE_FAULT  14     /* #PF on write */
#define CAUSE_PAGE_MODIFICATION 0xFF   /* software placeholder */

#define CAUSE_INTR_MASK         (1UL << 63)
#define CAUSE_CODE_MASK         ((1UL << 63) - 1)

/* LAPIC register offsets */
#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ESR       0x280
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PERF  0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0
#define LAPIC_LVT_MASKED (1 << 16)

/* Timer frequency (TSC approx 1GHz on QEMU) */
#define CLINT_TIMER_FREQ   1000000000UL

/* Page table constants (4-level) */
#define ARCH_PT_LEVELS     4
#define ARCH_PT_ROOT_LEVEL 3
#define ARCH_PT_BITS       9
#define ARCH_PT_ENTRIES    512
#define ARCH_PT_USER_START 0
#define ARCH_PT_USER_END   256

/* sstatus bits (RISC-V compat aliases for arch-agnostic code) */
#define SSTATUS_SIE     (1UL << 9)
#define SSTATUS_SPIE    (1UL << 9)
#define SSTATUS_SPP     0
#define SSTATUS_FS_OFF    0
#define SSTATUS_FS_INITIAL 0
#define SSTATUS_FS_CLEAN   0
#define SSTATUS_FS_DIRTY   0
#define SSTATUS_FS_MASK    0

extern uint64_t boot_pgdir[512];

#endif
