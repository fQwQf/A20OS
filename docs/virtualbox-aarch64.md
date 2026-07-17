# A20OS on VirtualBox ARM64

VirtualBox ARM64 support now includes the PCI discovery and storage path needed
to leave serial-only bring-up. A20OS is packaged as a standard ARM64 UEFI
removable-media disk and uses the firmware's ACPI tables to discover PCIe.

## Status

| Component | Status | Notes |
|-----------|--------|-------|
| ARM64 UEFI boot disk | Implemented | `make vbox-image-aarch64` creates a FAT disk with `EFI/BOOT/BOOTAA64.EFI`. |
| UEFI loader | Implemented | Loads the flat kernel at `0x08080000`, exits boot services, disables the firmware MMU, and enters A20OS. |
| Early page tables | Implemented | Maps VirtualBox RAM at `0x08000000` as normal memory and high platform MMIO as device memory. |
| Serial console | Implemented, hardware validation needed | PL011 is configured at the observed address `0xFFDDF000`. |
| GIC | Implemented, hardware validation needed | GICv3 distributor and redistributor support replaces the previous invalid GICv2 CPU-interface code. |
| Timer | Software fallback | VirtualBox traps both ARM generic-timer interfaces at EL1; early boot uses a non-preemptive software counter pending device discovery. |
| ACPI/PCIe discovery | Implemented | The UEFI loader passes ACPI RSDP; the board parses MCFG and enumerates PCIe ECAM. |
| VirtIO-SCSI disk | In progress, build-verified | The driver initialises control, event, and request queues and uses PCI BAR mappings. It still needs a target VBox boot log proving the boot LUN and FAT mount. |
| VirtIO GPU/input/network | Not used by the standard ARM VM | VirtualBox ARM exposes VMSVGA plus USB HID, rather than VirtIO GPU/input. |
| VMSVGA display | Target-detected, validation ongoing | SVGAv3 `15ad:0406` is detected on VBox ARM. The driver uses the device-reported framebuffer offset and pitch rather than treating the complete VRAM BAR as scanout. |
| USB HID input | Not implemented | The standard ARM VM supplies USB keyboard/mouse behind xHCI; the existing virtio-input driver cannot receive them. |
| Serial recovery console | Implemented | Remains available when the disk or graphics path cannot initialize. |

The UEFI loader and handoff are regression-tested with AAVMF on QEMU. A real
VirtualBox ARM64 host is still required to validate the VirtualBox-specific
UART and GIC addresses. The current development machine is ARM64 WSL2 and does
not have `VBoxManage` installed, so it cannot perform that hardware-model test.

## Build

Install an AArch64 cross compiler and `mtools`. On Debian or Ubuntu:

```bash
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu mtools parted
make vbox-image-aarch64
```

The result is:

```text
.kernel-build/aarch64-virtualbox-aarch64-both-dev/a20os-vbox-aarch64.img
```

It is a 256 MiB GPT disk with a FAT32 EFI System Partition containing both
`EFI/BOOT/BOOTAA64.EFI` and the A20OS `/bin` root filesystem. It is not an
x86 GRUB ISO.

## Run in VirtualBox

Use VirtualBox 7.2 or newer on an ARM64 host. Create an ARM64 VM with at least
1 GiB RAM and EFI enabled, then attach the generated image as the first hard
disk. Do not enable Secure Boot.

During validation, configure UART1 as a TCP server
instead of a file: file mode records output but cannot carry keyboard input.
With the VM powered off, on Windows PowerShell:

```powershell
$vm = "A20"
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm $vm `
  --uart1 0x03f8 4 `
  --uartmode1 tcpserver 5555
```

Start the VM, then connect a terminal client to `127.0.0.1:5555`, for example
`telnet 127.0.0.1 5555` or PuTTY configured for Raw/TCP. The serial bring-up
image provides a kernel-resident recovery console if normal userspace cannot
start. A successful PCI and VirtIO-SCSI probe instead mounts `/bin` and starts
the normal A20OS userspace shell. Keep the serial connection open for the
first boot: it reports the ACPI MCFG window, PCI devices, VirtIO-SCSI capacity,
and filesystem mount result.

After updating this image format, rebuild it and replace the VDI attached to
the VM; converting an older raw image does not add the required GPT partition.
VirtualBox ARM normally uses a VirtIO-SCSI controller. Leave that default
controller in place unless your particular VirtualBox build requires another
one.

For the graphical desktop image, build this target instead. It is an
engineering image, not a claim that USB input is already supported:

```bash
make vbox-gui-image-aarch64
```

It produces `a20os-vbox-aarch64-gui.img` in the same build directory. The
normal image deliberately starts a text shell; the GUI image contains the
`/etc/a20-gui` marker so `/bin/init` starts `/bin/desktop` after `/bin` mounts
while retaining the serial shell on UART1.
Keep VMSVGA selected in VirtualBox.  SVGAv3 uses an explicit update command,
so a successful graphics probe is reported as `[GPU] SVGAv3 ready` on serial.

With `VBoxManage`, attach the disk to the controller selected for the ARM VM.
VirtualBox 7.2 commonly creates a `VirtioSCSI` controller, so a Windows
PowerShell example is:

```bash
VBoxManage storageattach "A20OS ARM64" --storagectl "VirtioSCSI" \
    --port 0 --device 0 --type hdd \
    --medium "C:\\Users\\super\\Downloads\\a20os-vbox-aarch64-gpt.vdi"
```

If the VM uses a differently named controller, obtain its name with
`VBoxManage showvminfo "A20OS ARM64" --machinereadable` and use that name.
The firmware only needs to see the EFI System Partition and its standard
`EFI/BOOT/BOOTAA64.EFI` path.

Expected first-stage output is:

```text
A20OS: loading kernel
======================================
    A20OS Kernel
======================================
```

If the UEFI message appears but no kernel serial output follows, the firmware
handoff worked and the remaining issue is the VirtualBox PL011 address or serial
configuration. If the UEFI message does not appear, verify EFI is enabled,
Secure Boot is disabled, and the image is first in the boot order.

## Verified development path

The loader can be tested independently against QEMU's AAVMF by building the
QEMU board at its RAM address:

```bash
make ARCH=aarch64 BOARD=qemu-virt-aarch64 BRINGUP=1 kernel-only
make ARCH=aarch64 BOARD=qemu-virt-aarch64 BRINGUP=1 \
    VBOX_AARCH64_LOAD_ADDRESS=0x40080000ULL \
    .kernel-build/aarch64-qemu-virt-aarch64-both-bringup/a20os-vbox-aarch64.img
```

This test has reached `System ready (bringup, no userspace)` under AAVMF.

## Validation Notes

The ARM64 VirtualBox hardware model is not installed on this development host,
so the image is build-verified but must still be run on the target ARM machine.
The PCI transport currently uses polling; this keeps storage and display
independent of VirtualBox-specific MSI/INTx routing while ACPI interrupt
controller parsing is added. Input is a separate missing xHCI/USB-HID driver,
not a VirtIO-input configuration problem. The PCI enumerator prints each
device and its BARs; include those lines with the serial log. If the serial log says `ACPI MCFG
unavailable`, include that log together with `VBox.log`: it means the firmware
did not publish ACPI through the standard UEFI configuration table.
