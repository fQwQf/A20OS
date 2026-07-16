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
#define GICC_BASE          (0xFCD40000UL + PAGE_OFFSET)
#define VIRTIO_BASE        0x0UL

#define UART0_IRQ          33U

#endif /* _VBOX_AARCH64_PLATFORM_H */
