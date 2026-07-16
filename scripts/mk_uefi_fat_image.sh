#!/bin/sh
# Create a GPT disk with a UEFI System Partition containing BOOTAA64.EFI.
set -eu

efi_image=${1:-}
output=${2:-}

if [ -z "$efi_image" ] || [ -z "$output" ]; then
    echo "Usage: $0 <BOOTAA64.EFI> <output.img>" >&2
    exit 1
fi
if [ ! -s "$efi_image" ]; then
    echo "Error: EFI image missing or empty: $efi_image" >&2
    exit 1
fi
for command_name in parted mformat mmd mcopy; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "Error: $command_name not found (install parted and mtools)" >&2
        exit 1
    }
done

mkdir -p "$(dirname "$output")"
rm -f "$output"
dd if=/dev/zero of="$output" bs=1M count=64 status=none

# VirtualBox's ARM UEFI firmware discovers a conventional EFI System
# Partition more reliably than a whole-disk FAT superfloppy, particularly
# when the disk is exposed through VirtIO-SCSI.  Keep one MiB at each end for
# GPT metadata/alignment and put a FAT32 ESP in between.
parted -s "$output" mklabel gpt
parted -s "$output" mkpart ESP fat32 1MiB 63MiB
parted -s "$output" set 1 esp on

esp_offset=$((1024 * 1024))
esp_image="$output@@$esp_offset"
mformat -i "$esp_image" -F -v A20OS ::
mmd -i "$esp_image" ::/EFI ::/EFI/BOOT
mcopy -i "$esp_image" "$efi_image" ::/EFI/BOOT/BOOTAA64.EFI
echo "VirtualBox ARM64 GPT/UEFI disk created: $output"
