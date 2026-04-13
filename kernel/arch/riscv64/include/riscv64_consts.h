#ifndef _ARCH_RISCV64_CONSTS_H
#define _ARCH_RISCV64_CONSTS_H

#define PHYS_MEMORY_BASE   0x80000000UL
#define PHYS_MEMORY_END    0x88000000UL
#define KERNEL_ENTRY       0x80200000UL

#define UART0_BASE         0x10000000UL
#define CLINT_BASE         0x02000000UL
#define VIRTIO_BASE        0x10001000UL
#define PLIC_BASE          0x0C000000UL
#define UART0_IRQ          10

#define PLIC_PRIORITY      (PLIC_BASE + 0x0000UL)
#define PLIC_PENDING       (PLIC_BASE + 0x1000UL)
#define PLIC_SENABLE(h)    (PLIC_BASE + 0x2080UL + (uint64_t)(h) * 0x100UL)
#define PLIC_SPRIORITY(h)  (PLIC_BASE + 0x201000UL + (uint64_t)(h) * 0x2000UL)
#define PLIC_SCLAIM(h)     (PLIC_BASE + 0x201004UL + (uint64_t)(h) * 0x2000UL)

#define CLINT_MTIME        (CLINT_BASE + 0xBFF8UL)
#define CLINT_MTIMECMP(h)  (CLINT_BASE + 0x4000UL + ((unsigned long)(h) * 8))
#define CLINT_TIMER_FREQ   10000000UL

#endif /* _ARCH_RISCV64_CONSTS_H */
