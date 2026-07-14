# STM32F103 port

The STM32F103 target is an ARMv7-M, Cortex-M3, NOMMU bring-up profile.
It deliberately reuses A20OS architecture and board boundaries:

- `kernel/arch/armv7m`: vector table, reset path, CPU primitives, exception
  frame definitions, firmware hooks, and compiler runtime helpers.
- `kernel/platform/stm32f103`: USART1, SysTick, NVIC-facing board operations,
  and fault diagnostics.
- `kernel/mcu`: the small-memory kernel profile and allocator.

The first milestone does not build the full process, VFS, network, or VM
subsystems. STM32F103 parts normally provide only 20 KiB SRAM and no MMU, so
those facilities require an embedded configuration layer rather than merely
new CPU and device drivers. The current image proves reset/data/BSS setup,
UART output, IRQ entry, a 1 kHz SysTick, WFI idle, and dynamic allocation.

## Build

```sh
make stm32f103-bringup
```

Outputs:

```text
.kernel-build/armv7m-both-bringup-nommu-stm32f103-f64k-r20k/kernel.elf
.kernel-build/armv7m-both-bringup-nommu-stm32f103-f64k-r20k/kernel.bin
```

The binary is linked for flash address `0x08000000`. Flash it with OpenOCD,
ST-Link, or another STM32 programmer. USART1 uses PA9/PA10 at 115200 8N1.
The reset-clock implementation intentionally uses the 8 MHz HSI clock.

For the Prechin Xuanwu board with STM32F103ZET6, build the 512 KiB Flash /
64 KiB SRAM layout:

```sh
make stm32f103-xuanwu
```

Build both the no-peripheral baseline and the full Xuanwu image with:

```sh
make check-stm32f103
```

The resulting image is:

```text
.kernel-build/armv7m-both-bringup-nommu-stm32f103-f512k-r64k/kernel.bin
```

With a CMSIS-DAP probe connected over SWD and OpenOCD installed:

```sh
make flash-stm32f103-xuanwu
```

The default OpenOCD interface is `interface/cmsis-dap.cfg`, using SWD at
1 MHz. It can be overridden when needed:

```sh
make flash-stm32f103-xuanwu \
    STM32_OPENOCD_ADAPTER_KHZ=400
```

The PZ CMSIS-DAP firmware does not reliably complete OpenOCD's generic
`reset halt` sequence. The Makefile therefore halts through the Cortex-M
Debug Halting Control and Status Register, writes and verifies flash, then
restores MSP and PC from the flash vector table before resuming. A specific probe can be selected with
`STM32_CMSIS_DAP_SERIAL=<serial>`.

The Xuanwu board's LED0 on PB5 is used as a visible heartbeat. It is active
low, turns on during early boot, and toggles every 500 ms after SysTick starts.
The onboard 16-bit FSMC LCD is also initialized during boot. Its backlight is
enabled on PB0 when that signal is populated. The panel reset is wired to the
board reset rather than a GPIO. The firmware first shows red/green/blue
self-test frames before drawing an A20OS status page with a live uptime
counter. The driver follows the vendor example's default HX8357D-compatible
3.5-inch panel path. This panel transfers each RGB565 pixel as two 8-bit
writes despite being connected through the STM32's 16-bit FSMC bus.

All Xuanwu peripherals are optional at runtime. LCD, external SRAM, TF card,
and touch initialization failures are reported over USART1 and then ignored;
the kernel still reaches the SysTick/WFI heartbeat loop. The plain
`stm32f103-bringup` target compiles the same platform manager with all
Xuanwu-specific probes disabled, which is the no-peripheral build gate.

The onboard TF socket uses the STM32 SDIO pins `PC8-PC12` and `PD2`. The
driver supports SDSC and SDHC/SDXC cards, 1-bit fallback and 4-bit transfer,
single-sector reads/writes, the generic `block_dev_t` interface, FAT32 boot
sector detection, volume label reporting, periodic health checks, removal
handling, and automatic reprobe after insertion. The MCU profile does not yet
link the full A20OS VFS, so this milestone exposes a real block device rather
than mounting it into a process-visible namespace.

The 3.5-inch panel carries an XPT2046-compatible resistive touch controller:
`PB1=T_CLK`, `PB2=T_DOUT`, `PB10=T_DIN`, `PF10=T_PEN`, and `PF11=T_CS`.
Touch is polled every 20 ms with trimmed sampling, noise rejection, smoothing,
and configurable axis calibration. Since XPT2046 has no readable device ID,
an idle panel and a missing panel are both harmlessly represented as an armed
interface until `T_PEN` is asserted.

The LCD UI has touch-selectable `STATUS`, `TF CARD`, and `TOUCH` pages. The
storage page shows capacity, FAT32 state, SDIO bus width, and volume label.
The touch page is a small drawing pad with continuous strokes and a `CLEAR`
button, so panel orientation and calibration can be checked without a serial
terminal. The status rows for TF and touch also act as page shortcuts.

The onboard 1 MiB asynchronous SRAM is probed on FSMC Bank1 NOR/SRAM3 at
`0x68000000`. A small independent allocator is available through
`stm32_extsram_alloc()` and `stm32_extsram_free()` for future framebuffer,
cache, or filesystem working-set use without consuming the internal 64 KiB
SRAM. The MCU `kmalloc()` implementation also uses it automatically when the
internal heap cannot satisfy an allocation, while retaining internal SRAM as
the fast first choice.

STM32CubeProgrammer can also write the binary at address `0x08000000`, or
write the ELF directly. After reset, USART1 prints on PA9 at 115200 8N1;
receive is PA10. Keep BOOT0 low for normal flash boot.

## QEMU smoke test

```sh
make run-stm32f103-qemu
```

QEMU's `stm32vldiscovery` model has 128 KiB flash and 8 KiB SRAM, so this
target uses a separate build directory and memory layout from the physical
STM32F103C8-style image.

## Hardware smoke test

After flashing, the expected behavior is:

1. LED0 starts lit and then toggles every 500 ms.
2. USART1 prints the status of each optional peripheral and one tick per
   second.
3. The LCD shows external SRAM, TF/FAT32, touch, and uptime status.
4. The bottom `STATUS`, `TF CARD`, and `TOUCH` buttons switch pages; drawing
   on the touch pad leaves a cyan stroke and `CLEAR` erases it.
5. Touching the panel shows coordinates and emits `[TOUCH] down`/`[TOUCH] up`
   messages.
6. Inserting or removing a TF card is detected within five or two seconds,
   respectively, without rebooting.

Use a FAT32-formatted TF card for the filesystem metadata check. Raw and
non-FAT32 cards still initialize as block devices.

## Next milestones

1. Add real PendSV context switching and a Cortex-M thread stack ABI.
2. Decide whether applications run privileged in a flat address space or use
   the Cortex-M MPU for coarse isolation.
3. Add an MCU configuration profile for scheduler, IPC, and a small VFS that
   can mount the existing TF `block_dev_t`.
4. Add GPIO, EXTI, SPI, I2C, and SPI-flash drivers under `kernel/drivers`.
5. Move SDIO transfers to DMA and use external SRAM for the block cache.
