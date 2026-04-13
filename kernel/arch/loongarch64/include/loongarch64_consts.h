#ifndef _ARCH_LOONGARCH64_CONSTS_H
#define _ARCH_LOONGARCH64_CONSTS_H

#define PHYS_MEMORY_BASE   0x80000000UL
#define PHYS_MEMORY_END    0x88000000UL
#define KERNEL_ENTRY       0x80200000UL

#define UART0_BASE         0x1FE001E0UL
#define CLINT_BASE         0x02000000UL
#define VIRTIO_BASE        0x10001000UL

/* IRQ controller MMIO window (virt board mapping). */
#define UART0_IRQ          10

/*
 * QEMU loongarch64 virt:
 *   UART device        : 0x1fe001e0
 *   LS7A PCH-PIC MMIO  : 0x10000000 - 0x10000fff
 *   External IRQ path  : PCH-PIC -> EXTIOI -> CPU HWI
 *
 * This file only implements:
 *   - single core
 *   - UART external interrupt
 *   - minimal platform IRQ controller glue
 */

/* -------------------- Platform wiring -------------------- */

/* LS7A PCH-PIC source irq for UART on QEMU virt */
#define LS7A_UART_SRC_IRQ        2U

/* Route that PCH-PIC source to EXTIOI input 2 as well */
#define EXTIOI_UART_IRQ          2U

/* -------------------- LS7A PCH-PIC MMIO -------------------- */
/*
 * QEMU doc says the LS7A interrupt controller is at 0x10000000-0x10000fff,
 * and offsets 0 / 0x20 / 0x60 / 0x80 / 0x380 / 0x3a0 / 0x3e0 work.
 * UART support requires:
 *   - unmask pin2 at reg 0x20
 *   - configure trigger mode at 0x60
 *   - route byte at 0x202 to target EXTIOI irq
 */
#define LS7A_PCH_PIC_BASE        0x10000000UL

#define LS7A_PCH_PIC_ISR         (LS7A_PCH_PIC_BASE + 0x0000UL) /* pending/status */
#define LS7A_PCH_PIC_MASK        (LS7A_PCH_PIC_BASE + 0x0020UL) /* unmask via bit clear/enable model */
#define LS7A_PCH_PIC_HTMSI_VEC   (LS7A_PCH_PIC_BASE + 0x0200UL) /* byte routing map: src irq -> extioi irq */
#define LS7A_PCH_PIC_TRIG        (LS7A_PCH_PIC_BASE + 0x0060UL) /* trigger mode */
#define LS7A_PCH_PIC_INTCLR      (LS7A_PCH_PIC_BASE + 0x0080UL) /* needed for edge-trigger mode */

/* -------------------- EXTIOI IOCSR -------------------- */
/*
 * QEMU doc states:
 *   - route group regs : IOCSR 0x14c0 - 0x14c7
 *   - per-irq cpu route: IOCSR 0x1c00 - 0x1cfe
 *   - ack isr          : IOCSR 0x1800 -
 *
 * Enable regs are not spelled out in the excerpt, but table 11-10 is noted
 * as working as expected. On QEMU/Loongson docs this is typically the enable
 * bank at 0x1600 + group*4.
 */
#define IOCSR_EXTIOI_EN_BASE     0x1600U   /* inferred from extioi table layout */
#define IOCSR_EXTIOI_ISR_BASE    0x1800U
#define IOCSR_EXTIOI_ROUTE_BASE  0x14c0U
#define IOCSR_EXTIOI_COREMAP_BASE 0x1c00U

/* Route extioi groups [0..31], [32..63], ... to HWI0 */
#define EXTIOI_ROUTE_TO_HWI0     0x0U

/* node0/core0 */
#define EXTIOI_ROUTE_CORE0       0x1U

#define CLINT_MTIME        (CLINT_BASE + 0xBFF8UL)
#define CLINT_MTIMECMP(h)  (CLINT_BASE + 0x4000UL + ((unsigned long)(h) * 8))
#define CLINT_TIMER_FREQ   10000000UL

#endif /* _ARCH_LOONGARCH64_CONSTS_H */
