# Sipeed LicheeRV Nano (SophGo SG2002)

`ARCH=riscv64 BOARD=licheerv-nano`

The cheapest board in the tree at roughly USD 9–14, and the one that needs the
least new code: its core is a T-Head C906 that upstream describes as
`rv64imafdc` with `mmu-type riscv,sv39`, which is exactly what the riscv64 build
already targets.

**Build-verified only. Not run on hardware.** That build claim is now
reproduced on every commit by `make check-lao64-board-builds`, so the board cannot
silently stop compiling. Everything below marked
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

## Deploying

`tools/a20` is the deployment tool. There is no make command in this section,
and that is deliberate: the board has no block driver, so there is no card to
write and nothing for `dd` to put an image on. The artifact is the kernel plus
the commands that hand it to the boot chain already on the board.

### What you get

```sh
tools/a20 package licheerv-nano
```

writes `build/licheerv-nano/handoff/`:

| File | What it is |
|---|---|
| `kernel.bin` | the raw kernel, to be loaded at the address in `uboot.cmd` |
| `uboot.cmd` | the exact lines to run at the U-Boot prompt |
| `README.md` | the same instructions, shipped with the artifact |

The load address is **not** written in this document or in the instance. `a20
package` reads it out of the first `PT_LOAD` header of the kernel it just built,
so a board whose linker script relocates the image cannot end up with a stale
address here. For this board it resolves to `0x80200000`, which is the riscv64
`PHYS_BASE`.

No device tree is shipped, on purpose. See [Why no DTB](#why-no-dtb) below.

### Getting it onto the board

1. Boot the board's own way: vendor FSBL, then OpenSBI, then U-Boot. The FSBL
   and the `fip.bin` packaging come from the SophGo tool; keep the FSBL on the
   card, because the card is the only way back.
2. Interrupt U-Boot's autoboot.
3. Tell U-Boot where the host is, then load the kernel:

   ```text
   setenv serverip <your-host-ip>
   setenv ipaddr <board-ip>
   tftp 0x80200000 kernel.bin
   ```

   Load `kernel.bin` from `build/licheerv-nano/handoff/`. Transfer it however
   your host reaches the board — TFTP as above, or a USB stick and
   `fatload usb 0:1 0x80200000 kernel.bin`.
4. Start the kernel:

   ```text
   booti 0x80200000 - 0x80200000
   ```

   The `-` is the device tree pointer. Leaving it empty is correct: U-Boot passes
   its own board DTB in `a1` itself.

### Verifying

`tools/a20 deploy licheerv-nano` does the build and packaging, prints the
handoff commands, and then attaches the console to check the result. To just
watch an already-running board:

```sh
tools/a20 console licheerv-nano
```

The console is `/dev/ttyUSB0` at 115200 8N1. Override the node if your adapter
enumerates differently — `ls /dev/ttyUSB*` before plugging in and after.

A boot counts only if **both** of these appear:

| Line | What it proves |
|---|---|
| `[FDT] RAM range ...` | the firmware handed over the device tree |
| `System ready` | the kernel reached userspace |

`console_check` in the instance waits for exactly these two, so `a20 console`
distinguishes a boot from a hang. If you see the RAM-window fallback message
instead of a range, U-Boot did not pass `a1` and the load is wrong — not the
kernel.

Then it runs `cat /etc/os-release` and `poweroff`, and checks for `A20OS` and
`poweroff`.

### NOMMU

riscv64 is in `NOMMU_SUPPORTED_ARCHES`, and a tighter footprint is one override
away:

```sh
tools/a20 package licheerv-nano -- NOMMU=1
```

The VisionFive 2 linker script relocates its image to `0x40200000` for its own
DRAM window; this board's DRAM is at `0x80000000`, so the arch default stands
and no NOMMU-specific script is needed. Note that NOMMU cannot run `mksh` or
anything else that needs `fork`/`mmap`, so the in-console checks above are the
ceiling on a NOMMU build.

### Why no DTB

Every board in this family boots through the vendor FSBL and U-Boot, and U-Boot
passes its own board DTB in `a1`. That DTB is the authoritative description of
the board, and it is what `riscv64_memory_init()` reads for RAM and what the
platform device tree walker reads for devices. Shipping a second copy from the
kernel tree would be a second thing that has to be kept in sync with the
firmware, and nothing in the tree would notice when it drifted.

The consequence is real and worth stating: if your U-Boot was built without a
DTB, or passes a DTB that describes a different RAM window, the kernel will fall
back to the board's compiled-in window and the `[FDT] RAM range` line will not
appear.

### If it does not boot

Work down this list; it is ordered by what actually fails first.

1. **Nothing at all on the console.** Wrong UART or wrong pinmux. UART0 is
   `0x04140000` on the GPIO header; the FSBL is what releases its clock, so a
   silent console usually means the FSBL did not run, not that the kernel is
   wrong.
2. **U-Boot banner, then silence.** The load address. Compare what `uboot.cmd`
   says against `bdinfo` on your board.
3. **`[FDT] memory node unavailable`.** U-Boot did not pass `a1`. Check that
   your U-Boot was built with `CONFIG_OF_BOARD` or that you did not point `booti`
   at a DTB you truncated.
4. **RAM range is wrong.** U-Boot's DTB disagrees with the board. That is a
   firmware problem; `riscv64_memory_init()` is faithfully reporting what it was
   handed.
5. **Hangs after `System ready`.** You are past bring-up. That is no longer this
   document's subject; see `porting-guide.md`.

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

1. `tools/a20 package licheerv-nano` produces
   `build/licheerv-nano/handoff/kernel.bin`.
2. Load it through U-Boot once the FSBL is up, as [Deploying](#deploying)
   describes.
3. Confirm `System ready` on UART0 at 115200 8N1, GPIO header.
4. Confirm the DTB reached the kernel — `riscv64_memory_init()` prints
   `[FDT] RAM range ...`; a silent fallback to the board window means U-Boot
   did not pass `a1`.
5. Record the acceptance table from `porting-guide.md`. Until step 3 happens,
   the only true statement about this port is that it builds.

Keep the recovery path intact: keep the vendor FSBL on the card, and do not
overwrite it. The card is the only way back.
