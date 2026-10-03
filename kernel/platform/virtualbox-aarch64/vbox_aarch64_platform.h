#ifndef _VBOX_AARCH64_PLATFORM_H
#define _VBOX_AARCH64_PLATFORM_H

/* VirtualBox ARM64 observed memory map (SwiftOS / VirtualBox firmware logs). */

#define PHYS_MEMORY_BASE   0x08000000UL
#define PHYS_MEMORY_END    0x28000000UL
#define KERNEL_ENTRY       0x08080000UL

/* PAGE_OFFSET and USER_VA_LIMIT are deliberately not redefined here.
 * kernel/arch/aarch64/include/platform.h sets both above this include, in an
 * #ifdef CONFIG_NOMMU block, with the same MMU values and the right NOMMU ones.
 * Repeating the MMU pair here is what made `ARCH=aarch64
 * BOARD=virtualbox-aarch64 NOMMU=1` fail to compile: the NOMMU pass defined
 * PAGE_OFFSET as 0, then this header redefined it, and -Werror rejected it. */

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
