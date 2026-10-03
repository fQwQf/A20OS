#ifndef _ARCH_RISCV64_PLATFORM_H
#define _ARCH_RISCV64_PLATFORM_H

#include "core/types.h"

/* Board memory map.  The defaults below are QEMU virt's; a board with a
 * different RAM window or MMIO layout supplies its own header instead of
 * editing this file, which docs/platforms/porting-guide.md reserves for
 * instruction-set mechanisms only. */
#if defined(CONFIG_BOARD_LICHEERV_NANO)
#include "licheerv_nano_platform.h"
#elif defined(CONFIG_BOARD_MILK_V_DUO)
#include "milk_v_duo_platform.h"
#else
/* Physical memory layout (QEMU virt) */
#define PHYS_MEMORY_BASE   0x80000000UL
#define PHYS_MEMORY_END    0xC0000000UL
/* Some hosts start RISC-V guests with as much as 16 GiB of RAM. */
#define PHYS_MEMORY_MAX_END (PHYS_MEMORY_BASE + (16UL << 30))
#define KERNEL_ENTRY       0x80200000UL
#endif

#ifdef CONFIG_NOMMU
#define PAGE_OFFSET        0x0UL
#else
#define PAGE_OFFSET        0xFFFFFFC000000000UL
#endif
#define USER_VA_LIMIT      0x4000000000UL

size_t arch_ram_range_count(void);
int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end);
void riscv64_memory_init(void);
int riscv64_fdt_has_isa_extension(const char *extension);
uint64_t riscv64_fdt_timebase_freq(void);
/* Returns how many platform devices the tree yielded, so a board can tell an
 * absent tree from a tree that simply describes nothing it has a driver for. */
int riscv64_fdt_enumerate_platform_devices(void);

#if !(defined(CONFIG_BOARD_LICHEERV_NANO) || defined(CONFIG_BOARD_MILK_V_DUO))
/* MMIO base addresses.  The SophGo boards supply their own UART0_BASE,
 * PLIC_BASE and UART0_IRQ from the header above and have neither a PCIe
 * controller nor a virtio MMIO transport, so those constants are deliberately
 * left undefined rather than defaulted to the QEMU virt addresses. */
#define UART0_BASE         (0x10000000UL + PAGE_OFFSET)
#define VIRTIO_BASE        (0x10001000UL + PAGE_OFFSET)
#define PLIC_BASE          (0x0C000000UL + PAGE_OFFSET)
#define PCIE_ECAM_BASE     (0x30000000UL + PAGE_OFFSET)
#define PCIE_MMIO_BASE     0x40000000UL
#define PCIE_MMIO_SIZE     0x40000000UL
#define UART0_IRQ          10
#define CLINT_BASE         (0x02000000UL + PAGE_OFFSET)
#endif

/* PLIC register offsets */
#define PLIC_PRIORITY      (PLIC_BASE + 0x0000UL)
#define PLIC_PENDING       (PLIC_BASE + 0x1000UL)
#define PLIC_SENABLE(h)    (PLIC_BASE + 0x2080UL + (uint64_t)(h) * 0x100UL)
#define PLIC_SPRIORITY(h)  (PLIC_BASE + 0x201000UL + (uint64_t)(h) * 0x2000UL)
#define PLIC_SCLAIM(h)     (PLIC_BASE + 0x201004UL + (uint64_t)(h) * 0x2000UL)

/* CLINT timer.  A board header may publish its own CLINT_BASE and
 * ARCH_TIMER_FREQ; QEMU virt's 10 MHz applies otherwise.  riscv64 reads the
 * `time` CSR and programs the comparator through SBI, so CLINT_MTIME and
 * CLINT_MTIMECMP have no riscv64 reader today. */
#define CLINT_MTIME        (CLINT_BASE + 0xBFF8UL)
#define CLINT_MTIMECMP(h)  (CLINT_BASE + 0x4000UL + ((unsigned long)(h) * 8))
#ifndef ARCH_TIMER_FREQ
#define CLINT_TIMER_FREQ   10000000UL
#define ARCH_TIMER_FREQ    CLINT_TIMER_FREQ
#endif

/* Exception codes (scause) */
#define IRQ_S_SOFT         1
#define IRQ_S_TIMER        5
#define IRQ_S_EXT          9
#define CAUSE_ECALL_U      8
#define CAUSE_INSN_MISALIGNED   0
#define CAUSE_INSN_FAULT        1
#define CAUSE_ILLEGAL_INSN      2
#define CAUSE_BREAKPOINT        3
#define CAUSE_LOAD_MISALIGNED   4
#define CAUSE_LOAD_FAULT        5
#define CAUSE_STORE_MISALIGNED  6
#define CAUSE_STORE_FAULT       7
#define CAUSE_INSN_PAGE_FAULT   12
#define CAUSE_LOAD_PAGE_FAULT   13
#define CAUSE_STORE_PAGE_FAULT  15
#define CAUSE_PAGE_MODIFICATION 0xFF  /* Not a real RISC-V exception; placeholder to satisfy trap.c */

#define CAUSE_INTR_MASK         (1UL << 63)
#define CAUSE_CODE_MASK         ((1UL << 63) - 1)

/* SIE interrupt enable bits */
#define SIE_SSIE       (1L << 1)
#define SIE_STIE       (1L << 5)
#define SIE_SEIE       (1L << 9)

/* sstatus bits */
#define SSTATUS_SIE     (1UL << 1)
#define SSTATUS_SPIE    (1UL << 5)
#define SSTATUS_SPP     (1UL << 8)
#define SSTATUS_FS_OFF    (0UL << 13)
#define SSTATUS_FS_INITIAL (1UL << 13)
#define SSTATUS_FS_CLEAN   (2UL << 13)
#define SSTATUS_FS_DIRTY   (3UL << 13)
#define SSTATUS_FS_MASK    (3UL << 13)

extern uint64_t boot_pgdir[512];

/*
 * Clear the boot-time identity map (root entries [0,256): low MMIO slot plus
 * the RAM megapages).  It is transitional: secondary harts still execute a
 * handful of instructions at identity addresses right after writing satp in
 * .enable_mmu, so the map must stay valid (and executable) until all
 * secondaries are past that point.  kernel_main calls this after
 * smp_boot_secondaries().  Defined in arch/riscv64/mm/kwx.c.
 */
void arch_unmap_boot_identity(void);

#endif
