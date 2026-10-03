#ifndef _RK3328_PLATFORM_H
#define _RK3328_PLATFORM_H

/*
 * Rockchip RK3328 board memory map.
 *
 * Addresses transcribed from upstream Linux at commit
 * 551c722f40809618230001baccf219193e22fc5a:
 *   arch/arm64/boot/dts/rockchip/rk3328.dtsi    uart0, gmac2io, emmc, gic, cru, grf
 *   arch/arm64/boot/dts/rockchip/rk3328-rock64.dts
 *
 * Rock64 aliases ethernet0 to gmac2io; the NanoPi R2S aliases it to gmac1
 * instead, so a board aiming at the R2S must change GMAC_BASE.  This header
 * follows Rock64.
 *
 * Unverified: DRAM_BASE.  The Rock64 device tree declares no memory node -- the
 * bootloader passes the layout -- so the window below is the RK3328 DDRC
 * aperture from the SoC's memory map rather than something read off a board.
 * Confirm it against your U-Boot before trusting a boot past the console.
 */

#define PHYS_MEMORY_BASE   0x00000000UL
/* 2 GiB aperture.  Rock64 ships 1/2/4 GiB and the NanoPi R2S 1 GiB; the real
 * size comes from the bootloader, so this is a window and not a claim. */
#define PHYS_MEMORY_END    0x80000000UL
/* U-Boot's load address for the board's kernel. */
#define KERNEL_ENTRY       0x00080000UL

/* serial@ff110000: "rockchip,rk3328-uart", "snps,dw-apb-uart" -- a 16550. */
#define UART0_BASE         (0xFF110000UL + PAGE_OFFSET)
/* GIC_SPI 55, and a GIC SPI id is 32 + the DT number. */
#define UART0_IRQ          87U

/* interrupt-controller@ff811000: "arm,gic-400", i.e. GICv2, which is what
 * kernel/arch/aarch64/include/platform.h's GICD_BASE/GICC_BASE pair drives. */
#define GICD_BASE          (0xFF811000UL + PAGE_OFFSET)
#define GICC_BASE          (0xFF812000UL + PAGE_OFFSET)

/* ethernet@ff540000 (gmac2io): "rockchip,rk3328-gmac", driven upstream by
 * dwmac-rk.c -- the same Synopsys DWMAC family kernel/drivers/net/ls2k_gmac.c
 * targets.  GIC_SPI 24.  Not yet driven; see kernel/platform/rk3328/board.c. */
#define GMAC_BASE          (0xFF540000UL + PAGE_OFFSET)
#define GMAC_SIZE          0x10000UL
#define GMAC_IRQ           56U

/* mmc@ff520000: "rockchip,rk3328-dw-mshc", "rockchip,rk3288-dw-mshc" -- a
 * DesignWare MSHC in SD/eMMC mode, upstream driven by dwmci-mshc.  GIC_SPI 14.
 * Not yet driven: A20OS's dw_sdio.c targets the older DesignWare SDIO/MMC. */
#define EMMC_BASE          (0xFF520000UL + PAGE_OFFSET)
#define EMMC_SIZE          0x4000UL
#define EMMC_IRQ           46U

/* Rockchip clock controller and generic register file.  The console UART's
 * clocks are already enabled by U-Boot, which is why early_init() does not
 * touch the CRU; anything else added here does need it. */
#define CRU_BASE           (0xFF440000UL + PAGE_OFFSET)
#define GRF_BASE           (0xFF100000UL + PAGE_OFFSET)

/* RK3328's DZI.  QEMU virt's 62.5 MHz does not apply and the generic timer
 * would run 2.6x wrong, so this has to be the board's value. */
#define ARCH_TIMER_FREQ    24000000UL

/* No PCIe on the RK3328, and no virtio MMIO window.  PCIE_ECAM_BASE and
 * VIRTIO_BASE are deliberately left undefined: defining them as 0 would let a
 * caller map physical address 0 as a device window. */

#endif /* _RK3328_PLATFORM_H */
