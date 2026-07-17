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
The generic bring-up target uses the 8 MHz HSI clock. The Xuanwu target enables
its 8 MHz HSE and switches to the rated 72 MHz HSE×9 PLL clock before UART and
SysTick initialization; it falls back to HSI if HSE or PLL startup fails. APB1
runs at 36 MHz and APB2 at 72 MHz. The USART1 driver derives its APB2 clock from
the live RCC registers and
computes BRR instead of assuming the vendor example's 72 MHz clock. Its
RXNE interrupt feeds a 128-byte receive ring and clears parity, framing,
noise, and overrun conditions using the required SR/DR read sequence.

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
`PB1=T_CLK`, `PB2=T_DOUT`, `PF9=T_DIN`, `PF10=T_PEN`, and `PF11=T_CS`.
Touch is polled every 20 ms with trimmed sampling, noise rejection, smoothing,
and configurable axis calibration. Since XPT2046 has no readable device ID,
an idle panel and a missing panel are both harmlessly represented as an armed
interface until `T_PEN` is asserted.

The four yellow keys are also available when touch is absent. The vendor
mapping is `PA0` active-high plus `PE4`, `PE3`, and `PE2` active-low. They are
polled every 20 ms with debounce and exposed as up, left, down, and right
events (`PA0`, `PE2`, `PE3`, `PE4`, respectively). Left/right cycle through
the `STATUS`, `MEM`, `TF`, `BT`, and `INPUT TEST` pages. Up/down select
status rows; right opens the selected memory, storage, Bluetooth, or input
row. On the input page up moves a visible test cursor and down clears the
canvas.

The onboard photoresistor is sampled on `PF8` through `ADC3 channel 6`,
matching the vendor light-sensor experiment. Ten real 12-bit conversions are
taken for each reading; the highest and lowest samples are discarded, and the
remaining values are averaged and low-pass filtered. The LCD system page
shows both the measured ADC value and the vendor-compatible relative light
level from 0 to 100. This is deliberately not labelled as lux because the
resistor network has no factory photometric calibration.

LCD backlight control drives the board's documented active-high `LCD_BL`
signal on `PB0`. Brightness modulation and automatic brightness are disabled;
PB0 remains high so the panel stays at its normal hardware brightness. The
photoresistor continues to be sampled and displayed independently.

The memory page uses runtime measurements rather than treating the Makefile
capacity settings as hardware facts. On Xuanwu, Flash capacity comes from
the STM32 factory size register. Internal SRAM capacity is derived from the
live DBGMCU device identifier and factory Flash density; an unknown device
is explicitly shown using the linked-layout fallback instead of claiming a
silicon measurement. Firmware Flash use comes from the final linked
load-image boundary. Internal RAM use combines the static data/BSS extent,
live allocator metadata and allocations, and a stack high-water mark
measured from a reset-time fill pattern. External SRAM capacity is found by
probing successive address-line boundaries until the first mirrored address,
then checked with saved-and-restored patterns across the detected span. Its
live allocator use is shown separately only after those probes succeed.

The PZ-HC05 Bluetooth module is connected to `USART3` on `PB10/PB11` at
38400 8N1. `PA4` drives the module's KEY input and remains low for transparent
data mode; `PA15` receives the dedicated PIO9 connection-state output after
JTAG-only pins are released while SWD remains enabled. PA15 uses a pull-up,
and a link is accepted only after the signal remains high for 500 ms; this
prevents an absent module or an LED-style pulse from being reported as a
connection.

USART1, USART2, and USART3 share the STM32 UART register driver. It derives their
peripheral clocks from the live RCC clock tree, computes and reports the actual
BRR value, resets each USART before use, configures 8N1 TX/RX pins, drains
hardware error conditions, and controls the corresponding NVIC line. USART3
explicitly clears its remap bits so the module always uses the Xuanwu
`PB10/PB11` routing even if a debugger or previously flashed application left
AFIO in a different state.

The two radios operate concurrently. The directly inserted HC-05 keeps the
board socket described above. The ESP8266 connected with Dupont wires uses
`USART2` (`PA2` TX to ESP RX, `PA3` RX from ESP TX), with `PC6` driving
CH_PD/EN and `PC7` driving RESET. Both modules and the MCU must share ground;
the ESP8266 supply must provide adequate 3.3 V current. USART2 and USART3 have
independent receive interrupts and ring buffers, so WiFi probing cannot take
ownership of or reconfigure the Bluetooth UART.

The ESP8266 is an ESP8266EX with 1 MiB Flash and official Nano AT v1.7.4.0
(SDK v3.0.4) at 115200 8N1. The nonblocking probe asserts RESET for 500 ms,
waits two seconds, then tries each candidate baud up to three times, starting
at 115200. Hardware testing confirmed reset control and both UART directions.
The last failure was a defective Dupont wire between PA2 and the module RX;
replacing it completed the AT hardware link. AP association and the proxy TCP
connection remain separate configuration/integration steps.

Board early-init keeps PA4 low so the module boots into discoverable data
mode. Configuration follows the supplied PZ sample's runtime AT sequence
exactly: it tries 9600 repeatedly, raises KEY only while transmitting the
command, then lowers KEY before waiting for the response. After the first
valid reply it tests whether this firmware expects subsequent commands with
KEY low, pulsed per command, or held high. It then checks the other supported
data rates. It writes and
reads back slave role (`ROLE=0`),
arbitrary-peer pairing (`CMODE=1`), device name, PIN, and transparent UART
settings, then issues `AT+RESET`, lowers KEY, and enters transparent slave
waiting mode. KEY is lowered immediately after the reset command leaves
USART3 so the module samples data mode during reboot; reset is also issued
when a vendor-specific command cannot be read back, preventing a detected
module from being stranded in undiscoverable AT mode. An interface is shown
as ready only after the real module has
electrically driven PB11 or answered AT commands; `WAITING` still requires
the verified slave configuration and successful reset. This distinguishes
"module present but AT dialect unknown" from "module absent" without
claiming an unverified configuration. Defaults are `KasaneTeto`, PIN `2233`,
UUID `0x1101`, and 38400 baud. They can be changed while building:

```sh
make stm32f103-xuanwu \
    STM32_BT_NAME=MY-BOARD STM32_BT_PIN=6789 \
    STM32_BT_UUID=1101 STM32_BT_BAUD=38400
```

The service is classic Bluetooth SPP. Standard HC-05 firmware fixes the UUID
to `0x1101` (`00001101-0000-1000-8000-00805F9B34FB`) and does not expose
`AT+UUID`; compatible firmware that implements the command is written and
read back. A non-`1101` requested UUID is therefore reported as unverified
on a standard HC-05 instead of being presented as real module state. USART3
receives into an interrupt-driven ring buffer and frames input on either a
newline or a 20 ms idle gap. The Bluetooth page displays the verified role,
name, PIN, UUID source, link state and byte counters. The test transmit
button succeeds only while PA15 reports a real connection.

Before enabling USART3, the driver also tests whether PB11 follows an
internal pull-up and pull-down. If it does, the module TX path is floating
and boot reports `rx=floating`. On Xuanwu this normally means the P10
`USART3_TX`/`W_TX` and `USART3_RX`/`W_RX` jumper caps are missing or placed
incorrectly, the module is inserted backwards, or it is not powered. A
powered idle module normally reports `rx=driven-high`. Full AT probing is not
repeated automatically because an absent module can otherwise block the
peripheral service task for several seconds. Use `bt retry` after
changing module wiring or power.

USART1 also provides an interactive bring-up prompt at 115200 8N1:

```text
uart
perf
sd retry
bt
bt probe
bt at AT
bt at AT+ROLE?
bt at AT+NAME?
bt at AT+PSWD?
bt at AT+UART?
```

Every automatic and manual AT exchange prints its USART3 baud, KEY mode,
escaped command, escaped raw reply, byte count, and hardware error count.
`reply=<none>` means PB11 delivered no UART byte; repeated `\xNN` data with
errors indicates a baud, wiring, or signal-quality problem; a readable
`ERROR` indicates that the module is present but does not implement that AT
command dialect. The `uart` command prints both ports' measured peripheral
clock, requested and actual baud, BRR, receive-interrupt state, and accumulated
hardware errors. After Bluetooth initialization, USART3 should report
`initialized=1`, `requested=38400`, `rx-irq=1`; a nonzero error count is direct
evidence that PB11 is toggling but the received framing is invalid.

SysTick derives its 1 kHz reload from the live RCC HCLK rather than assuming
the reset-default 8 MHz clock. The `perf` command reports HCLK/PCLK values and
the measured maximum duration of the peripheral service loop, light sampling,
manual Bluetooth retries, and SD-card checks. Optional HC-05 and absent TF-card
initialization are only retried by explicit UART commands so command timeouts
cannot periodically freeze input and display updates.

The MCU profile now links the generic A20OS process scheduler. ARMv7-M
`__switch` saves and restores `r4-r11`, SP, the return address, the task
pointer, and CONTROL. The physical board runs the peripheral service and the
USART1 diagnostic console as separate kernel tasks; QEMU substitutes a
scheduler probe task. Tasks are designed to yield cooperatively with
`proc_yield()`. `PendSV_Handler` remains a no-op, so SysTick/PendSV time-slice
preemption is not yet implemented. MCU-only batch limits, a 256-entry PID
namespace, and compatibility stubs reduce the generic scheduler footprint.
The 8 KiB QEMU profile currently has only about 2.6 KiB heap after boot and
panics with `cannot create diagnostic task` before the scheduler probe runs;
the 64 KiB Xuanwu image links successfully but still requires a hardware
round-trip test.

The LCD UI remains touch-selectable when a working panel is fitted. The
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
2. USART1 prints boot status and peripheral events. The old once-per-second
   `[TICK]` line is disabled to keep the console quiet.
3. The LCD shows external SRAM, TF/FAT32, direction-key, and uptime status.
4. The yellow direction keys emit `[KEY]` messages. Left/right switch pages,
   while up/down control the current page.
5. If touch is available, the bottom `STATUS`, `MEM`, `TF`, `BT`, and
   `INPUT` buttons switch pages; drawing leaves a cyan stroke and `CLEAR`
   erases it.
6. Touching the panel shows coordinates and emits `[TOUCH] down`/`[TOUCH] up`
   messages.
7. The `BT` page reports the HC-05 link state. Pair a phone at 38400 8N1,
   send text from a Bluetooth serial app, and press up to transmit the
   `A20OS HC05 TEST` response.
8. Cover and uncover the onboard photoresistor. The `LIGHT` ADC/level values
   change while the panel backlight remains fixed on.
9. Inserting or removing a TF card is detected within five or two seconds,
   respectively, without rebooting.

Use a FAT32-formatted TF card for the filesystem metadata check. Raw and
non-FAT32 cards still initialize as block devices.

## Next milestones

1. Extend the working cooperative ARMv7-M kernel-thread switch to SysTick/
   PendSV preemption; `__switch` currently handles voluntary `proc_yield()`
   context switches while `PendSV_Handler` is still a no-op.
2. Decide whether applications run privileged in a flat address space or use
   the Cortex-M MPU for coarse isolation.
3. Replace the MCU compatibility stubs with the required IPC and small-VFS
   implementations, then mount the existing TF `block_dev_t`.
4. Add GPIO, EXTI, SPI, I2C, and SPI-flash drivers under `kernel/drivers`.
5. Move SDIO transfers to DMA and use external SRAM for the block cache.
