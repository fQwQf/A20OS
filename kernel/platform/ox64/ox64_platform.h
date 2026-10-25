#ifndef _OX64_PLATFORM_H
#define _OX64_PLATFORM_H

/* BL808 D0 C906: OpenBouffalo Linux bl808.dtsi and PINE64 Ox64 DTS.
 * PSRAM is 64 MiB, not QEMU virt DDR. M0/E907 is a separate M-mode core. */
#define PHYS_MEMORY_BASE    0x50000000UL
#define PHYS_MEMORY_END     0x53f00000UL /* last MiB belongs to M0 firmware */
#define PHYS_MEMORY_MAX_END 0x53f00000UL
#define KERNEL_ENTRY        0x50200000UL
#define UART0_BASE          (0x30002000UL + PAGE_OFFSET)
#define UART0_IRQ           20U
#define PLIC_BASE           (0xe0000000UL + PAGE_OFFSET)
#define CLINT_BASE          (0xe4000000UL + PAGE_OFFSET)
#define ARCH_TIMER_FREQ     1000000UL

#endif
