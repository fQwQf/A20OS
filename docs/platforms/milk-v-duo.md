# Milk-V Duo (SophGo CV1800B)

`ARCH=riscv64 BOARD=milk-v-duo`

Roughly USD 10–13. Same SophGo IP family and the same boot chain as the
[LicheeRV Nano](licheerv-nano.md), with one decisive difference: **this board
has 64 MiB of RAM**, not 256 MiB. That makes it the second target, not the
first.

**Build-verified only. Not run on hardware.**

## Hardware facts

| Item | Value | Source |
|---|---|---|
| SoC | SophGo CV1800B | `cv1800b.dtsi` |
| CPU | T-Head C906 @1 GHz + C906 @700 MHz + 8051 | Sipeed wiki |
| ISA | `rv64imafdc`, `mmu-type riscv,sv39` | `cv180x-cpus.dtsi` |
| RAM | **64 MiB** integrated DDR2 at `0x80000000`, size `0x4000000` | `cv1800b.dtsi` `memory@80000000` |
| Timebase | 25 MHz | `cv180x-cpus.dtsi` |
| UART0 | `0x04140000`, IRQ 44 | `cv180x.dtsi` `serial@4140000` |
| MAC | `0x04070000`, IRQ 31, `"snps,dwmac-3.70a"`, internal PHY | `cv180x.dtsi` `ethernet@4070000` |
| SD | `0x04310000`, IRQ 36, `sophgo,cv1800b-dwcmshc` | `cv180x.dtsi` `mmc@4310000` |
| eMMC | `0x04300000`, IRQ 34, `sophgo,cv1800b-dwcmshc` | `cv181x.dtsi` `emmc@4300000` |
| PLIC | `0x70000000`, 64 MiB, 101 interrupts | `cv1800b.dtsi` |
| CLINT | `0x74000000` | `cv1800b.dtsi` |
| Boot medium | TF card or SD-NAND | Sipeed wiki |

The peripherals are identical to the LicheeRV Nano's because both inherit
`cv180x.dtsi`; only RAM size and the top-level compatible differ. U-Boot's own
boot log for this board reads `DRAM: 63.3 MiB`, which matches the 64 MiB DT
node and confirms the window.

## The RAM budget is the whole story

The kernel binary is 2.1–3.8 MiB depending on profile. What does not fit is
everything else: the page cache, the lwIP stacks, the VFS, and an apk userland.

On 64 MiB:

- `BRINGUP=1` and `RAMFS_USER=1` are required.
- Swap must be off. `Makefile:187` already refuses `CONFIG_SWAP` for the
  architectures without a swap PTE encoding, and riscv64 does have one, so
  pass `SWAP=0` explicitly.
- A full `image-world` rootfs will not build or will not mount. Do not try
  `make image-world` on this board.

```sh
make ARCH=riscv64 BOARD=milk-v-duo ABI=linux BRINGUP=1 RAMFS_USER=1 SWAP=0 \
     kernel-only
```

If the goal is "cheapest board that boots A20OS", buy the LicheeRV Nano: same
$10–14 class, four times the RAM, identical drivers. Choose the Duo only when
the 64 MiB part is the actual constraint.

## Boot chain

```text
CV1800B boot ROM  →  vendor FSBL  →  OpenSBI  →  kernel
```

Identical to the LicheeRV Nano. mainline U-Boot has `configs/milkv_duo_defconfig`
with `CONFIG_CLK_SOPHGO_CV1800B`, `CONFIG_MMC` and `CONFIG_CMD_MMC`, and
upstream Linux carries `arch/riscv/boot/dts/sophgo/cv1800b-milkv-duo.dts`.
Community work in July 2026 was switching the board to
`CONFIG_OF_UPSTREAM` with the upstream SophGo DT, so the DT A20OS eventually
sees is the upstream one.

`CONFIG_SYS_LOAD_ADDR=0x80080000` and `CONFIG_CUSTOM_SYS_INIT_SP_ADDR=0x82300000`
in that defconfig describe U-Boot's own scratch addresses, not the kernel load
address. **The kernel load address on this board is unverified** — see below.

## Unverified: the kernel load address

A20OS's riscv64 `KERNEL_ENTRY` and `PHYS_BASE` are compile-time constants, and
the arch default `0x80200000` is what QEMU virt uses. U-Boot honours
`kernel_addr` / a `load` address from the boot script or the bootflow, which
need not be `0x80200000`.

If the kernel hangs before the first `printf`, this is the first thing to check:
compare U-Boot's `kernel_addr` against `KERNEL_ENTRY`, and if they differ,
change `KERNEL_ENTRY` in the board's `<board>_platform.h` and the `PHYS_BASE` /
`VIRT_BASE` pair in a board `ldscript.ld` **together**. They must agree, or the
kernel jumps to an address it was not linked for.

This is stated as unverified because it cannot be settled without the board.

## Reuse, and what is missing

Identical to the LicheeRV Nano:

- shared `kernel/drivers/irqchip/plic.c` — the DTS says `thead,c900-plic`,
  which mainline binds to the standard SiFive PLIC driver
- no linker script override needed; DRAM is at `0x80000000`
- no architecture work; `rv64imafdc` matches the riscv64 build
- SBI for the timebase and for `poweroff`/`reboot`

Missing, both needing real drivers:

- **Storage.** `sophgo,cv1800b-dwcmshc` at `0x04310000` (SD) and `0x04300000`
  (eMMC, on the Duo 256M and Duo S). A20OS's `dw_sdio.c` covers the older
  DesignWare SDIO/MMC MSHC layout, not DWCMSH.
- **Ethernet.** `snps,dwmac-3.70a` with an internal PHY. The base Duo also
  needs an **external 1:1 transformer and RJ45**, which the board does not
  populate — several sellers ship the 64 MiB board without them, so a "no
  network" report on this board may be a missing transformer rather than a
  driver bug.
- **SMP.** Not described upstream, so `smp` is `NULL`. The Duo S's second
  core is documented by Sipeed rather than by a DT.

## Before claiming hardware verification

Same five steps as the LicheeRV Nano, with the load-address check moved to the
front because it is the most likely failure on this board. Keep the vendor FSBL
on the card.
