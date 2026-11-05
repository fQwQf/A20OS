# Board Flashing & Platform Bringup Utilities (`tools/boards/`)

This directory contains board-specific deployment, firmware flashing, boot bundle generation, and UART/OpenOCD handoff tools for supported physical targets.

## Supported Platforms

| Board / SoC | Architecture | Files | Capabilities |
|-------------|--------------|-------|--------------|
| **ESP32-C3** | RISC-V 32 | `esp32c3.py`, `check_esp32c3_rom.py` | esptool flashing, reset, and ROM image verification |
| **HiSilicon Hi3861** | RISC-V 32 | `hi3861.py`, `hi3861_flash.py` | Hupg image packaging, checksum calculation, and serial flashing |
| **STM32F103** | ARM Cortex-M3 | `stm32.py` | OpenOCD SWD flashing and ST-Link serial runner |
| **Bouffalo BL808 (Ox64)** | RISC-V 64 | `ox64.py` | Boot0 recovery, MBR SD partitioning, and UART bootloader |
| **Allwinner A64 (Pine A64)** | AArch64 | `pine_a64.py` | U-Boot SPL bundling and raw SD card layout composer |
| **Allwinner V3s (Lichee Pi Zero)** | ARMv7-A | `lichee_pi_zero.py` | U-Boot and uImage SD card preparer |
| **SOPHGO SG2002 (LicheeRV Nano)** | RISC-V 64 | `licheerv_nano.py` | Handoff packaging, U-Boot boot.scr creation, and SD flashing |
| **Loongson 2K0300** | LoongArch 64 | `ls2k0300_handoff.py`, `mk_ls2k0300_bundle.py` | USB volume staging, serial auto-boot, and TFTP staging |
| **Loongson 2K1000** | LoongArch 64 | `ls2k1000-uboot-stop.runscript` | Minicom automation script to halt U-Boot autoboot |
| **Bouffalo BL702 (M0Sense)** | RISC-V 32 | `m0sense_deploy_uf2.py`, `m0sense_recovery_assets.py`, `mk_m0sense_uf2.py` | UF2 volume deployment, recovery assets, and USB flashing |
| **Rockchip RK3566 (Quartz64 Zero)**| AArch64 | `quartz64_zero.py` | Firmware packaging, video configuration, and SD card writing |
| **Raspberry Pi 4** | AArch64 | `raspberrypi4_fixture.py`, `check-raspberrypi4.py`, `check-fdt-view.py` | DeviceTree fixture generator, hardware handoff validator, and FDT reader sanitizer check |
| **Huawei CT5100** | x86_64 | `collect_pc_windows.ps1` | PowerShell diagnostic hardware information collector |
