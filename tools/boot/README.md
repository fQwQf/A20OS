# Boot Media & Packaging Utilities

This directory contains bootloader wrappers, image packagers, and firmware reset helpers.

## Contents

- `kernel_bundle.py`: Validates Linux Image protocol handoff headers and generates platform-specific boot recipes.
- `mk_kernel_bundle.sh`: Shell wrapper around `kernel_bundle.py` for headless board bringup.
- `mk_grub_disk_image.sh`: Builds a bootable x86_64 GPT/FAT32 disk with GRUB EFI loader.
- `mk_grub_iso.sh`: Generates an x86_64 rescue CD ISO using GRUB Multiboot v1.
- `mk_uefi_fat_image.sh`: Assembles a dual-architecture GPT disk containing UEFI ESP and optional ext4 distro world.
- `shared-qemu-reset.S`: QEMU platform reset stub.
