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
| VirtIO-SCSI disk | Target-verified | The VBox `1af4:1048` controller discovers the boot LUN and mounts the GPT FAT32 partition at `/bin`. |
| Network | Target-detected, validation ongoing | VBox exposes Intel E1000 (`8086:100e`); the driver is integrated with lwIP and DHCP. |
| VMSVGA display | Target-verified, desktop validation ongoing | SVGAv3 `15ad:0406` is detected on VBox ARM. The driver uses the device-reported framebuffer offset and pitch and maps VRAM as Device memory. |
| USB HID input | Implemented, target validation needed | The Intel `8086:1e31` xHCI controller is polled; USB keyboard, relative mouse and VBox USB Tablet events feed `/dev/event0`. |
| Serial recovery console | Implemented | Remains available when the disk or graphics path cannot initialize. |
| Userspace and remote shell | Implemented | The MMU userspace reaches musl `init`/`mksh`; `telnetd` listens on TCP port 2323. Startup prints the exact `/init` size, ELF entry and FNV-1a hash to identify stale disks. |

The UEFI loader, PCI discovery, VirtIO-SCSI and SVGAv3 probe have been exercised
on VirtualBox ARM64. The same AArch64 MMU userspace image is also regression
tested on QEMU through `init`, `fork`/`exec`, and an interactive `mksh`.

## Build

Install an AArch64 cross compiler and `mtools`. On Debian or Ubuntu:

```bash
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu mtools parted
make vbox-image-aarch64
```

The result is a graphical image. It contains `/etc/a20-gui`, so PID 1 starts
`/bin/desktop`:

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
Do not run `convertfromraw` over an attached or existing VDI. Use a new output
name each time, which also avoids VirtualBox retaining the old medium UUID:

```powershell
$raw = "C:\Users\super\Downloads\a20os-vbox-aarch64.img"
$vdi = "C:\Users\super\Downloads\a20os-vbox-aarch64-20260717.vdi"
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" convertfromraw `
  $raw $vdi --format VDI
```

The build also creates `a20os-vbox-aarch64.img.sha256` and stores
`A20OS.MANIFEST` in the ESP. On a correct current image the serial log's
`[INIT] image` line and `file-entry` must match the newly built `/init`; an
entry from an older build proves that the VM is still attached to a stale VDI.
VirtualBox ARM normally uses a VirtIO-SCSI controller. Leave that default
controller in place unless your particular VirtualBox build requires another
one.

For a remote shell, keep the VM's Intel PRO/1000 MT Desktop adapter attached
to NAT and forward a host port to guest TCP 2323. With the VM powered off:

```powershell
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm $vm `
  --nic1 nat `
  --nictype1 82540EM `
  --natpf1 "a20-telnet,tcp,127.0.0.1,2323,,2323"
```

After `[telnetd] listening on port 2323` appears on serial, connect with
`telnet 127.0.0.1 2323` or PuTTY Raw/TCP. The service is intentionally
unauthenticated and should only be forwarded to loopback.

`vbox-gui-image-aarch64` remains available as an explicit graphical-image
target. VBox's xHCI USB keyboard and mouse are exposed through `/dev/event0`;
the serial and telnet shells remain available as recovery paths:

```bash
make vbox-gui-image-aarch64
```

It produces `a20os-vbox-aarch64-gui.img` in the same build directory. Both
graphical targets contain the `/etc/a20-gui` marker and retain the serial shell
on UART1. For a serial-only recovery disk, run `make vbox-text-image-aarch64`;
it produces `a20os-vbox-aarch64-text.img`.
Keep VMSVGA selected in VirtualBox.  SVGAv3 uses an explicit update command,
so a successful graphics probe is reported as `[GPU] SVGAv3 ready` on serial.
The red, green, blue and white bars are the driver's scanout self-test, not the
desktop. They should be replaced after userspace reports all of the following:

```text
[init] desktop queued: pid=2
[desktop] entered main
Framebuffer mapped: va=0x30000000 size=3145728 stride=4096
[desktop] framebuffer ready
Mission Control initialized, entering loop...
```

The graphical image starts the desktop with ordinary `fork`/`exec`. It does
not suspend PID 1 with `vfork`, because the VirtualBox timer fallback is
cooperative. After publishing a new child, the cooperative clone path performs
one explicit yield so the child can enter `exec` without waiting for a timer
interrupt that VirtualBox ARM does not provide.

The image target also compares the staged `/init` with the current MMU user
build before packaging.  This catches an interrupted/incremental build that
left a NOMMU `init` in the FAT image: that binary changes `fork()` to `vfork()`
and appears in the log as `clone begin: ... flags=0x4111`, where PID 1 waits
forever for a child which has not yet been scheduled.  A current graphical MMU
image reports `flags=0x11`, followed by `desktop queued` and
`[desktop] entered main`.

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

The PCI transports currently use polling; this keeps storage, display and xHCI
HID independent of VirtualBox-specific MSI/INTx routing while ACPI interrupt
controller parsing is added. The PCI enumerator prints each
device and its BARs; include those lines with the serial log. If the serial log says `ACPI MCFG
unavailable`, include that log together with `VBox.log`: it means the firmware
did not publish ACPI through the standard UEFI configuration table.
