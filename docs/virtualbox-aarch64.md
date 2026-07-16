# A20OS on VirtualBox ARM64

VirtualBox ARM64 support is at the **serial bring-up** stage. A20OS can be
packaged as a standard ARM64 UEFI removable-media disk and the firmware can
load the kernel. GUI, block storage, networking, and input are not yet claimed
as supported on this board.

## Status

| Component | Status | Notes |
|-----------|--------|-------|
| ARM64 UEFI boot disk | Implemented | `make vbox-image-aarch64` creates a FAT disk with `EFI/BOOT/BOOTAA64.EFI`. |
| UEFI loader | Implemented | Loads the flat kernel at `0x08080000`, exits boot services, disables the firmware MMU, and enters A20OS. |
| Early page tables | Implemented | Maps VirtualBox RAM at `0x08000000` as normal memory and high platform MMIO as device memory. |
| Serial console | Implemented, hardware validation needed | PL011 is configured at the observed address `0xFFDDF000`. |
| GIC | Implemented, hardware validation needed | GICv3 distributor and redistributor support replaces the previous invalid GICv2 CPU-interface code. |
| Timer | Software fallback | VirtualBox traps both ARM generic-timer interfaces at EL1; early boot uses a non-preemptive software counter pending device discovery. |
| GUI, disk, network, input | Not implemented | VirtualBox ARM device discovery and drivers remain future work. |

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

It is a 64 MiB GPT disk with a FAT32 EFI System Partition containing
`EFI/BOOT/BOOTAA64.EFI`. It is not an x86 GRUB ISO.

## Run in VirtualBox

Use VirtualBox 7.2 or newer on an ARM64 host. Create an ARM64 VM with at least
1 GiB RAM and EFI enabled, then attach the generated image as the first hard
disk. Do not enable Secure Boot. Start the VM and inspect its serial output.

After updating this image format, rebuild it and replace the VDI attached to
the VM; converting an older raw image does not add the required GPT partition.
VirtualBox ARM normally uses a VirtIO-SCSI controller. Leave that default
controller in place unless your particular VirtualBox build requires another
one.

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

## Remaining port work

The port is bootable but not feature-complete. The next work is to obtain the
ACPI tables or other device description supplied by VirtualBox, discover its
PCI host bridge, and then enable the actual block, network, framebuffer, and
input devices instead of assuming the QEMU `virt` layout.
