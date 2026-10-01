# Sipeed LicheeRV Nano (SophGo SG2002)

`ARCH=riscv64 BOARD=licheerv-nano`

The cheapest board in the tree at roughly USD 9–14, and the one that needs the
least new code: its core is a T-Head C906 that upstream describes as
`rv64imafdc` with `mmu-type riscv,sv39`, which is exactly what the riscv64 build
already targets.

**Build-verified only. Not run on hardware.** Everything below marked
unverified is unverified because this board has not been powered up under
A20OS.

## Hardware facts

All from upstream, not from a vendor document:

| Item | Value | Source |
|---|---|---|
| SoC | SophGo SG2002 | `sg2002.dtsi` |
| CPU | 1× T-Head C906 @1 GHz (+ C906 @700 MHz, not described upstream) | `cv180x-cpus.dtsi` |
| ISA | `rv64imafdc`, extensions `i m a f d c zicntr zicsr zifencei zihpm` | `cv180x-cpus.dtsi` |
| MMU | `riscv,sv39` | `cv180x-cpus.dtsi` |
| RAM | 256 MiB integrated DDR3 at `0x80000000`, size `0x10000000` | `sg2002.dtsi` `memory@80000000` |
| Timebase | 25 MHz | `cv180x-cpus.dtsi` `timebase-frequency` |
| UART0 | `0x04140000`, 256 bytes, IRQ 44 | `cv180x.dtsi` `serial@4140000` |
| MAC | `0x04070000`, IRQ 31, `"sophgo,cv1800b-dwmac", "snps,dwmac-3.70a"` | `cv180x.dtsi` `ethernet@4070000` |
| SD | `0x04310000`, IRQ 36, `"sophgo,sg2002-dwcmshc"` | `cv180x.dtsi` `mmc@4310000` |
| PLIC | `0x70000000`, 64 MiB, 101 interrupts | `sg2002.dtsi` `interrupt-controller@70000000` |
| CLINT | `0x74000000` | `sg2002.dtsi` `timer@74000000` |
| Pin mux | `0x03001000` (sys), `0x05027000` (rtc) | `sg2002.dtsi` |
| Clock | `0x03002000` | `sg2002.dtsi` |
| Boot medium | TF card, or SD-NAND under the TF slot | Sipeed wiki |

UART0 is `"snps,dw-apb-uart"` with `reg-shift = <2>` and `reg-io-width = <4>`:
a DesignWare APB UART in 16550 register layout, which is what every
architecture's `console.h` already declares.

Peripheral interrupts are `SOC_PERIPHERAL_IRQ(nr) = nr + 16`, so the DTS
interrupt numbers are 16 lower than the PLIC wire numbers. The board uses the
wire numbers.

## What the tree already had to reuse

- **The PLIC is the shared one.** The DTS says `"thead,c900-plic"`, which
  mainline Linux binds to `drivers/irqchip/irq-sifive-plic.c` — the standard
  SiFive PLIC, not a separate T-Head driver. `kernel/drivers/irqchip/plic.c`
  therefore applies unchanged. (Mainline flags
  `PLIC_QUIRK_EDGE_INTERRUPT` for this compatible because the C900 can emit
  edge-triggered interrupts; every interrupt in this DTS is
  `IRQ_TYPE_LEVEL_HIGH`, which takes the level path that needs no separate
  handling.)
- **No linker script override.** DRAM starts at `0x80000000`, which is where
  `kernel/arch/riscv64/boot/ldscript.ld` already points, so the arch default
  image is correct as-is.
- **No architecture work.** The ISA string matches the build's
  `-march=rv64imafdc_zicsr_zifencei -mabi=lp64` exactly.
- **SBI for time and power.** riscv64 reads the `time` CSR and programs the
  comparator through `firmware_set_timer()`; `CLINT_BASE` is not read by any
  riscv64 code, so the CLINT address above is recorded but unused.

## Boot chain

```text
CV1800B boot ROM  →  vendor FSBL (cv181x.bin)  →  OpenSBI  →  kernel
```

The FSBL initialises clocks and DRAM and loads OpenSBI; the vendor tool packs
FSBL + OpenSBI + U-Boot into a `fip.bin` placed on the FAT partition of a TF
card. The FSBL is freely published and **not signed** — unlike Amlogic, there
is no vendor-signed blob here.

mainline U-Boot has a board target, `configs/licheerv_nano_defconfig`, with
the ns16550 UART and the **Synopsys DesignWare MSHC** driver enabled. That is
worth knowing: it confirms both the console and the SD controller are on IP
A20OS already speaks.

## Building

```sh
make ARCH=riscv64 BOARD=licheerv-nano ABI=linux BRINGUP=1 kernel-only
```

NOMMU also builds, for a tighter footprint:

```sh
make ARCH=riscv64 BOARD=licheerv-nano NOMMU=1 BRINGUP=1 kernel-only
```

riscv64 is in `NOMMU_SUPPORTED_ARCHES` and `kernel/platform/visionfive2/`
already ships an `ldscript-nommu.ld` as the worked example. Note that the
VisionFive 2 linker script relocates the image to `0x40200000` for its own DRAM
window; this board's DRAM is at `0x80000000`, so the arch default load address
stands and no NOMMU-specific script is needed.

## What this port does and does not do

Does: RAM discovery narrowed by the firmware DTB, the shared PLIC, the SBI
timebase, console on UART0, `poweroff`/`reboot` through SBI SRST.

Does not, and each needs real driver work rather than a device table entry:

- **Storage.** The SD controller is a DesignWare CMSH
  (`sophgo,sg2002-dwcmshc`, driven by `sdhci-of-dwcmshc.c` upstream).
  A20OS's `dw_sdio.c` targets the older DesignWare SDIO/MMC MSHC register
  layout that the VisionFive 2's dw-mci uses. The two are close relatives but
  not close enough to pretend one driver covers both. This is the blocker for
  getting past a RAM-only boot.
- **Ethernet.** The MAC is `snps,dwmac-3.70a`, an older stmmac than the
  DWMAC1000 `ls2k_gmac.c` targets. It also has an internal PHY
  (`phy-mode = "internal"`). Only the `-E` and `-WE` variants populate the
  RJ45.
- **SMP.** The second C906 is not described in the upstream device tree, so
  `smp` is `NULL`. `porting-guide.md` is explicit that a board which cannot
  start secondaries must not supply a fake empty ops table.

## Before claiming hardware verification

1. `make ARCH=riscv64 BOARD=licheerv-nano BRINGUP=1 kernel-only` produces
   `kernel.bin`.
2. Pack it into the `fip.bin` with the vendor tool, or load `kernel.bin`
   through U-Boot once the FSBL is up.
3. Confirm `[INIT] System ready` on UART0 at 115200 8N1, GPIO header.
4. Confirm the DTB reached the kernel — `riscv64_memory_init()` prints
   `[FDT] RAM range ...`; a silent fallback to the board window means U-Boot
   did not pass `a1`.
5. Record the acceptance table from `porting-guide.md`. Until step 3 happens,
   the only true statement about this port is that it builds.

Keep the recovery path intact: keep the vendor FSBL on the card, and do not
overwrite it. The card is the only way back.
