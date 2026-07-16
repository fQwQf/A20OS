# A20OS on VirtualBox ARM64 (Apple Silicon)

This document collects the current state of running A20OS inside the ARM64 edition of **VirtualBox** on Apple Silicon hosts (M1/M2/M3 Macs). It is **not yet a complete porting guide** because VirtualBox ARM64 still has firmware-level limitations that prevent reliable bare-metal OS boot.

## QEMU `virt-aarch64` GUI (development target)

For day-to-day development, use QEMU's `virt` machine. The following targets are now available:

```bash
# Text-mode serial console
make ARCH=aarch64 run

# GUI with virtio-gpu / virtio-keyboard / virtio-mouse
make run-gui-aarch64
# or
make run-gui-arm64
```

The GUI target uses `-display gtk` and attaches virtio-mmio GPU/input devices on buses 5–7 alongside the existing virtio-blk (bus 0) and virtio-net (bus 4) devices.

> Note: after switching boards (e.g. from `qemu-virt-aarch64` to `virtualbox-aarch64`), run `make clean` or remove the per-board build directory so that object files compiled with the wrong `CONFIG_BOARD_*` macro are not reused. The build directory is now `.kernel-build/<arch>-<board>-<variant>/`.

## Current status

| Component | Status | Notes |
|-----------|--------|-------|
| Board skeleton | **Implemented** | `kernel/platform/virtualbox-aarch64/` compiles with `make ARCH=aarch64 BOARD=virtualbox-aarch64 BRINGUP=0 dev-build`. |
| Build-system multi-board support | **Implemented** | `kernel/arch/aarch64/include/platform.h` now includes a board-specific header; the Makefile picks a board-specific `ldscript.ld` when present. |
| Boot via UEFI | **Blocked** | Reported firmware issue: VirtualBox does not reliably launch `\EFI\BOOT\BOOTAA64.EFI` for custom guests. Observed on VirtualBox 7.2.x previews. |
| Entry page tables | **Not ported** | `kernel/arch/aarch64/boot/entry.S` is still hardcoded for QEMU `virt`: L1[0] maps PA 0x0 as device, L1[1] maps PA 0x40000000. This does not give the VirtualBox kernel (loaded at 0x08080000) normal memory attributes or map device MMIO at 0xFFDDF000. |
| UART (PL011) | **Not tested** | Address is wired to `0xFFDD_F000` in the board header, but the console will not work until the entry page tables are fixed. |
| GIC | **Not ported** | Observed at `0xFCD3_0000` (GICv3, unconfirmed). The existing A20OS GIC driver in `kernel/arch/aarch64/trap/irqchip.c` is GICv2-only. |
| RTC (PL031) | **Not ported** | Observed at `0xFFDD_E000`. |
| GPIO (PL061) | **Not ported** | Observed at `0xFFDD_D000`. |
| SATA/AHCI | **Unknown** | Storage controller for ARM64 guests is not documented; likely VirtIO-SCSI or AHCI. AHCI driver from x86_64 may need PCI/PCIe mapping. |
| Network | **Unknown** | Likely VirtIO-net or E1000. |
| GPU/framebuffer | **Not usable** | SwiftOS reports observed framebuffer size of `0x0`; graphics console is not expected to work. |
| Input | **Unknown** | May be USB, PL050 KMI, or absent. |

Sources:
- SwiftOS VirtualBox ARM documentation: https://swiftos.tech/docs/virtualbox and https://github.com/asaptf/swift-os/blob/main/docs/VIRTUALBOX.md
- VirtualBox ARM EFI firmware source: https://github.com/VirtualBox/virtualbox/blob/main/src/VBox/Devices/EFI/DevEFI-armv8.cpp
- VirtualBox EFI ARM RAM base fix: https://github.com/VirtualBox/virtualbox/commit/e91fd0c19bbf627d4b14a5232737a22ebabe0f46

## Memory map differences from QEMU `virt`

| Resource | QEMU `virt` (AArch64) | VirtualBox ARM64 observed |
|----------|----------------------|---------------------------|
| RAM base | `0x4000_0000` | `0x0800_0000` |
| Kernel physical base | `0x4008_0000` | `0x0808_0000` |
| PL011 UART | `0x0900_0000` | `0xFFDD_F000` |
| GIC | `0x0800_0000` / `0x0801_0000` | `0xFCD3_0000` |
| PL031 RTC | `0x0901_0000` | `0xFFDD_E000` |
| PL061 GPIO | `0x0903_0000` | `0xFFDD_D000` |
| EFI firmware | N/A | `0x0000_0000` – `0x07FF_FFFF` (flash region) |
| FDT/ACPI table region | `0x0808_0000` area | Inside descriptor region after firmware |

## Boot method

VirtualBox ARM64 uses a built-in UEFI firmware (`VBoxEFI-arm64.fd` / `VBoxEFIAArch64.fd`). The firmware reads a **VBox descriptor** at physical address `0xFFFF_0000` to locate the RAM base and then sets up the stack and DTB/ACPI tables.

A guest OS can boot by either:

1. **UEFI application**: Provide a bootable disk with `\EFI\BOOT\BOOTAA64.EFI`. This is the standard path, but the firmware reportedly does not launch this file for bare-metal/homebrew kernels on current VirtualBox ARM64 previews.
2. **Direct firmware load**: Not supported for custom ELF kernels; VirtualBox expects a UEFI-aware loader.

Because of this, the first practical porting step is a **UEFI bootloader** that can load the A20OS kernel image and hand off control, not a kernel that boots directly from firmware.

## What A20OS would need for a VirtualBox ARM64 board (and what is now in place)

A new board `kernel/platform/virtualbox-aarch64/` has been created and compiles. The remaining items are:

1. **Board-specific platform constants** — Done. `kernel/arch/aarch64/include/platform.h` selects `kernel/platform/virtualbox-aarch64/vbox_aarch64_platform.h` via `CONFIG_BOARD_VIRTUALBOX_AARCH64`, and the Makefile picks a board-specific `ldscript.ld` when present. The default `qemu-virt-aarch64` board is unchanged.
2. **PL011 UART driver** — Address is set to `0xFFDD_F000`. The existing driver uses `UART0_BASE`, so it compiles, but it cannot run until the entry page tables map that region.
3. **Entry page tables** — `kernel/arch/aarch64/boot/entry.S` still hard-codes QEMU `virt` mappings. It must be taught to give the kernel region normal-memory attributes for PA `0x08000000` and to map device MMIO above 1 GiB (or be replaced by a board-specific `entry.S`).
4. **GICv3 driver** — `kernel/arch/aarch64/trap/irqchip.c` is GICv2-only. VirtualBox likely needs a GICv3 driver at `0xFCD3_0000`.
5. **Generic timer and PL031 RTC** — Timer code is architecture-generic; PL031 support is missing.
6. **Block storage driver** — Either reuse the x86_64 AHCI driver if VirtualBox ARM64 exposes a PCI AHCI controller, or add a VirtIO-SCSI/VirtIO-blk transport that matches the ARM64 storage controller.
7. **Network** — Reuse the existing virtio-net driver if the NIC is VirtIO-net; otherwise add E1000.
8. **Input** — Likely need a USB xHCI driver or PL050 KMI driver; the PS/2 driver from x86_64 will not work on ARM64.
9. **Framebuffer** — Current evidence suggests there is no usable framebuffer for custom guests. Serial output via PL011 is the realistic first milestone.
10. **UEFI loader** — Required because the firmware does not reliably boot a custom ELF kernel directly. The kernel image must be packaged as a PE/COFF `BOOTAA64.EFI` or loaded by a separate UEFI application.

## Recommended first milestone

Do **not** target GUI first. Target this instead:

1. A UEFI loader that can read the kernel image from the EFI system partition and jump to it with the DTB address in `x0` (per the Linux AArch64 boot protocol).
2. A minimal `virtualbox-aarch64` board that prints "Hello from A20OS on VirtualBox ARM64" over the PL011 UART at `0xFFDD_F000`.
3. Once serial output works, bring up the GIC, timer, and block storage.
4. GUI comes last, and only if VirtualBox exposes a framebuffer device for custom guests.

## Why the board is not yet runnable

- The **entry page tables** in `kernel/arch/aarch64/boot/entry.S` are hardcoded for QEMU `virt` and do not give the VirtualBox kernel (PA `0x08080000`) normal-memory attributes or map device MMIO at `0xFFDDF000`.
- The existing **GIC driver** is GICv2-only, while VirtualBox ARM64 appears to use GICv3.
- The firmware boot path is reportedly unreliable on the only public test reports available (SwiftOS). Until VirtualBox ARM64 can consistently launch a custom `BOOTAA64.EFI`, a fully functional port is blocked by the host hypervisor, not by A20OS code.

## Files created/modified

Created:
- `kernel/platform/virtualbox-aarch64/vbox_aarch64_platform.h` — board memory map and device addresses.
- `kernel/platform/virtualbox-aarch64/board.c` — `board_config_t` with GIC and timer ops for the VirtualBox memory map.
- `kernel/platform/virtualbox-aarch64/ldscript.ld` — linker script with `PHYS_BASE = 0x08080000` and `VIRT_BASE = 0x0000008008080000`.

Modified:
- `Makefile` — uses a board-specific `ldscript.ld` when `kernel/platform/$(BOARD)/ldscript.ld` exists; otherwise falls back to the arch default.
- `kernel/arch/aarch64/include/platform.h` — board-selectable constants via `CONFIG_BOARD_VIRTUALBOX_AARCH64`.
- `kernel/main.c` — guards AHCI block-device fallback with `#ifdef CONFIG_X86_64` so it does not break non-x86_64 builds.

Still needed for a bootable port:
- `kernel/boot/uefi/` — UEFI loader and PE/COFF image support.
- `kernel/arch/aarch64/boot/entry.S` — board-aware early page tables (or a board-specific `entry.S`).
- `kernel/arch/aarch64/trap/irqchip.c` — GICv3 support.
- `kernel/drivers/char/uart.c` — may need PL031 clock-frequency handling once serial works.

## Next steps

1. Make `kernel/arch/aarch64/boot/entry.S` board-aware so it sets up correct page tables for VirtualBox RAM and MMIO.
2. Add GICv3 support to `kernel/arch/aarch64/trap/irqchip.c`.
3. Write a minimal UEFI loader for A20OS AArch64.
4. Re-test when VirtualBox ARM64 firmware reliably launches custom UEFI boot loaders.
