#!/bin/sh
# Create a GPT disk with a UEFI System Partition containing BOOTAA64.EFI.
set -eu

efi_image=${1:-}
output=${2:-}
rootfs_image=${3:-}

if [ -z "$efi_image" ] || [ -z "$output" ] || [ -z "$rootfs_image" ]; then
    echo "Usage: $0 <BOOTAA64.EFI> <output.img> <rootfs-fat32.img>" >&2
    exit 1
fi
if [ ! -s "$efi_image" ]; then
    echo "Error: EFI image missing or empty: $efi_image" >&2
    exit 1
fi
if [ ! -s "$rootfs_image" ]; then
    echo "Error: root filesystem image missing or empty: $rootfs_image" >&2
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
dd if=/dev/zero of="$output" bs=1M count=256 status=none

# VirtualBox's ARM UEFI firmware discovers a conventional EFI System
# Partition more reliably than a whole-disk FAT superfloppy, particularly
# when the disk is exposed through VirtIO-SCSI.  Keep one MiB at each end for
# GPT metadata/alignment and put a FAT32 ESP in between.
parted -s "$output" mklabel gpt
parted -s "$output" mkpart ESP fat32 1MiB 255MiB
parted -s "$output" set 1 esp on

esp_offset=$((1024 * 1024))
mformat -i "$output@@$esp_offset" -F -v A20OS ::

# mtools maps one image per drive.  Use a temporary drive map so the complete
# FAT root (including /init, /mksh, /etc and runtime libraries) can share the
# UEFI system partition with EFI/BOOT/BOOTAA64.EFI.
tmp_mtoolsrc=$(mktemp)
trap 'rm -f "$tmp_mtoolsrc"' EXIT HUP INT TERM
cat > "$tmp_mtoolsrc" <<EOF
drive a: file="$rootfs_image"
drive b: file="$output" offset=$esp_offset
EOF
MTOOLSRC="$tmp_mtoolsrc" mcopy -s 'a:/*' b:/
MTOOLSRC="$tmp_mtoolsrc" mmd b:/EFI b:/EFI/BOOT
MTOOLSRC="$tmp_mtoolsrc" mcopy -o "$efi_image" b:/EFI/BOOT/BOOTAA64.EFI
echo "VirtualBox ARM64 GPT/UEFI disk with A20OS root filesystem created: $output"
