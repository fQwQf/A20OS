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

## Next milestones

1. Add real PendSV context switching and a Cortex-M thread stack ABI.
2. Decide whether applications run privileged in a flat address space or use
   the Cortex-M MPU for coarse isolation.
3. Add an MCU configuration profile for scheduler, IPC, and a small RAM VFS.
4. Add GPIO, EXTI, SPI, I2C, and flash drivers under `kernel/drivers`.
5. Add OpenOCD flashing and hardware UART smoke-test scripts.
