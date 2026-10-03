#ifndef _MILK_V_DUO_PLATFORM_H
#define _MILK_V_DUO_PLATFORM_H

/*
 * Milk-V Duo (SophGo CV1800B) board memory map.
 *
 * Every address and size below is transcribed from upstream Linux, not from a
 * vendor document:
 *   arch/riscv/boot/dts/sophgo/cv1800b.dtsi       memory, plic, clint, pinctrl, clock
 *   arch/riscv/boot/dts/sophgo/cv180x.dtsi        uart0, gmac0, sdhci0/1, emmc
 *   arch/riscv/boot/dts/sophgo/cv181x.dtsi        emmc
 *   arch/riscv/boot/dts/sophgo/cv180x-cpus.dtsi   timebase-frequency, ISA, mmu-type
 * at commit 551c722f40809618230001baccf219193e22fc5a.
 *
 * Unverified: KERNEL_ENTRY.  A20OS's riscv64 load address is a compile-time
 * constant and this board's U-Boot kernel_addr has not been read off the
 * hardware.  The arch default 0x80200000 is kept because it is where every
 * other RISC-V port in this tree loads; if the board hangs before the first
 * printf, this is the first value to check.  See
 * docs/platforms/milk-v-duo.md.
 */

#define PHYS_MEMORY_BASE   0x80000000UL
/* 64 MiB SIP DDR2: reg = <0x80000000 0x4000000> in cv1800b.dtsi.  U-Boot's own
 * boot log for this board reports "DRAM: 63.3 MiB", which agrees. */
#define PHYS_MEMORY_END    0x84000000UL
/* 64 MiB is a hard ceiling: this board has no DIMM and no growth path, and an
 * allocator that believed QEMU virt's base + 16 GiB would hand out addresses
 * that do not exist.  The firmware DTB is clamped against this in
 * arch/riscv64/platform/fdt.c. */
#define PHYS_MEMORY_MAX_END 0x84000000UL

/* RAM starts at the arch default address, so the default load address and the
 * default linker script both apply unchanged. */
#define KERNEL_ENTRY       0x80200000UL

/* serial@4140000: "snps,dw-apb-uart", reg-shift 2, io-width 4 -- a DesignWare
 * APB UART in 16550 layout.  Size 0x100. */
#define UART0_BASE         (0x04140000UL + PAGE_OFFSET)
/* SOC_PERIPHERAL_IRQ(28) with SOC_PERIPHERAL_IRQ(nr) = nr + 16. */
#define UART0_IRQ          44U

/* interrupt-controller@70000000: "sophgo,cv1800b-plic", "thead,c900-plic",
 * reg = <0x70000000 0x4000000>, riscv,ndev = 101.  mainline binds
 * "thead,c900-plic" to its standard SiFive PLIC driver, so
 * kernel/drivers/irqchip/plic.c applies unchanged. */
#define PLIC_BASE          (0x70000000UL + PAGE_OFFSET)

/* timer@74000000.  Recorded for completeness: riscv64 reads the `time` CSR and
 * programs the comparator through SBI, so no riscv64 code dereferences
 * CLINT_BASE. */
#define CLINT_BASE         (0x74000000UL + PAGE_OFFSET)

/* /cpus timebase-frequency = <25000000> in cv180x-cpus.dtsi.  The board's
 * timer ops prefer the firmware DTB and fall back to this. */
#define ARCH_TIMER_FREQ    25000000UL

/* The CV1800B has no PCIe controller and no virtio MMIO transport, so
 * PCIE_ECAM_BASE, PCIE_MMIO_BASE and VIRTIO_BASE are deliberately absent.
 * Defining them as 0 would let a caller map address 0 as a device window. */

/* Recorded from cv181x.dtsi for the Duo 256M and Duo S, which populate eMMC at
 * mmc@4300000 (reg 0x04300000, IRQ 18 + 16 = 34, "sophgo,cv1800b-dwcmshc").
 * The 64 MiB Duo has no eMMC.  A20OS has no DWCMSH driver, so this is
 * documentation rather than a working resource. */
#define EMMC_BASE          (0x04300000UL + PAGE_OFFSET)
#define EMMC_IRQ           34U

#endif /* _MILK_V_DUO_PLATFORM_H */
