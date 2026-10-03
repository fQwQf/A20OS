#ifndef _SUN50I_H616_PLATFORM_H
#define _SUN50I_H616_PLATFORM_H

/*
 * Allwinner H616 / H618 board memory map (Orange Pi Zero 2 / Zero 3 / Zero 2W).
 *
 * Transcribed from upstream Linux at commit
 * 551c722f40809618230001baccf219193e22fc5a:
 *   arch/arm64/boot/dts/allwinner/sun50i-h616.dtsi                  gic, uart0/1, mmc0/1, emac0
 *   arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero3.dts
 *   arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dts
 *
 * A GIC SPI interrupt number is 32 + the number in the device tree, so the DT
 * values are converted here.
 *
 * Unverified: DRAM size.  None of these boards declares a memory node -- U-Boot
 * passes the layout -- and A20OS's aarch64 port has no DT-based RAM narrowing
 * (unlike riscv64's riscv64_memory_init), so PHYS_MEMORY_END below is the
 * window the allocator is given, not a measurement.  See
 * docs/platforms/sun50i-h616.md.
 */

#define PHYS_MEMORY_BASE   0x40000000UL
/* 1 GiB window.  The H618 ships 1/2/4 GiB and the Zero 2 512 MiB-2 GiB; a
 * bootloader that reports more than this needs this raised. */
#define PHYS_MEMORY_END    0x80000000UL
/* Where U-Boot leaves the kernel for these boards.  UNVERIFIED: the H616 SPL
 * loads at 0x40000000 and U-Boot's loadaddr decides the rest. */
#define KERNEL_ENTRY       0x40080000UL

/* interrupt-controller@3021000: "arm,gic-400", i.e. GICv2, which is what the
 * aarch64 GICD_BASE/GICC_BASE pair in this tree already drives. */
#define GICD_BASE          (0x03021000UL + PAGE_OFFSET)
#define GICC_BASE          (0x03022000UL + PAGE_OFFSET)

/* serial@5000000: "snps,dw-apb-uart", 0x400 bytes, GIC_SPI 0. */
#define UART0_BASE         (0x05000000UL + PAGE_OFFSET)
#define UART0_IRQ          32U

/* ethernet@5020000: "allwinner,sun50i-h616-emac0", 0x10000 bytes, GIC_SPI 14.
 * Upstream binds this to dwmac-sun8i.c, a stmmac variant, so it is the same
 * Synopsys DWMAC family ls2k_gmac.c targets.  Not yet driven here. */
#define GMAC_BASE          (0x05020000UL + PAGE_OFFSET)
#define GMAC_SIZE          0x10000UL
#define GMAC_IRQ           46U

/* mmc@4020000: "allwinner,sun50i-h616-mmc", GIC_SPI 35.  This is Allwinner's
 * own controller (upstream sunxi-mmc.c), not a DesignWare one, so A20OS's
 * dw_sdio.c does not apply and nothing mounts a card yet. */
#define SDC0_BASE          (0x04020000UL + PAGE_OFFSET)
#define SDC0_SIZE          0x1000UL
#define SDC0_IRQ           67U

/* H616 DZI.  The aarch64 QEMU default of 62.5 MHz is wrong here and the generic
 * timer would run 2.6x fast. */
#define ARCH_TIMER_FREQ    24000000UL

/* No PCIe on the H616 and no virtio MMIO window; PCIE_ECAM_BASE and
 * VIRTIO_BASE stay undefined so nothing maps address 0 as a device window. */

#endif /* _SUN50I_H616_PLATFORM_H */