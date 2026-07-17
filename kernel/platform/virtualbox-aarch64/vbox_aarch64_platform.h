#ifndef _VBOX_AARCH64_PLATFORM_H
#define _VBOX_AARCH64_PLATFORM_H

/* VirtualBox ARM64 observed memory map (SwiftOS / VirtualBox firmware logs). */

#define PHYS_MEMORY_BASE   0x08000000UL
#define PHYS_MEMORY_END    0x28000000UL
#define KERNEL_ENTRY       0x08080000UL

#define PAGE_OFFSET        0x0000008000000000UL
#define USER_VA_LIMIT      0x0000004000000000UL

#define UART0_BASE         (0xFFDDF000UL + PAGE_OFFSET)
#define GICD_BASE          (0xFCD30000UL + PAGE_OFFSET)
#define GICR_BASE          (0xFCD40000UL + PAGE_OFFSET)
#define GICC_BASE          GICR_BASE
#define VIRTIO_BASE        0x0UL

#define CONFIG_AARCH64_GICV3 1

/* Reserved for the physical generic-timer PPI; VBox currently traps it. */
#define IRQ_S_TIMER        30U
#define UART0_IRQ          33U

/* The ECAM/MMIO windows are supplied by ACPI MCFG and PCI BARs at runtime. */
#define VBOX_PCI_MAX_BUS    256U

#endif /* _VBOX_AARCH64_PLATFORM_H */
