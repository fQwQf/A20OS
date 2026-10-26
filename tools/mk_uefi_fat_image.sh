#!/bin/sh
# Create a GPT disk with a UEFI System Partition containing an A20OS EFI loader.
# Serves both architectures: the ESP entry name is the 4th argument, so BOOTAA64.EFI
# for aarch64 and BOOTX64.EFI for x86_64 produce the same layout.
set -eu

efi_image=${1:-}
output=${2:-}
rootfs_image=${3:-}
efi_name=${4:-BOOTAA64.EFI}
world_image=${5:-}

if [ -z "$efi_image" ] || [ -z "$output" ] || [ -z "$rootfs_image" ]; then
    echo "Usage: $0 <loader.efi> <output.img> <rootfs-fat32.img|-> [esp-entry-name] [world-ext4.img]" >&2
    exit 1
fi
# BOOTX64.EFI is the x86_64 loader, BOOTAA64.EFI the aarch64 one. The manifest
# records which, so a reader of a finished disk is not told "ARM64" about a disk
# that boots x86_64.
case "$efi_name" in
    BOOTX64.EFI)  arch_slug=x86_64;  loader_key=bootx64 ;;
    BOOTAA64.EFI) arch_slug=aarch64; loader_key=bootaa64 ;;
    *) arch_slug=unknown; loader_key=$(printf '%s' "$efi_name" | tr 'A-Z.' 'a-z_') ;;
esac
if [ ! -s "$efi_image" ]; then
    echo "Error: EFI image missing or empty: $efi_image" >&2
    exit 1
fi
if [ "$rootfs_image" != "-" ] && [ ! -s "$rootfs_image" ]; then
    echo "Error: root filesystem image missing or empty: $rootfs_image" >&2
    exit 1
fi
world_mb=0
if [ -n "$world_image" ]; then
    # A distro remains an ext4 filesystem, not files copied into a FAT root.
    # Validate before creating/truncating any output.
    world_mb=$(python3 - "$world_image" <<'PY'
import os, struct, sys
path = sys.argv[1]
size = os.path.getsize(path)
with open(path, 'rb') as source:
    source.seek(1024)
    sb = source.read(1024)
if len(sb) != 1024 or sb[56:58] != b'\x53\xef':
    sys.exit('Error: world image is not ext4')
log = struct.unpack_from('<I', sb, 24)[0]
blocks = struct.unpack_from('<I', sb, 4)[0]
if struct.unpack_from('<I', sb, 96)[0] & 0x80:
    blocks |= struct.unpack_from('<I', sb, 0x150)[0] << 32
if log > 6 or not blocks or blocks * (1024 << log) > size:
    sys.exit('Error: world filesystem exceeds its image')
print((size + 1048575) // 1048576)
PY
    )
fi
for command_name in parted mformat mmd mcopy sha256sum awk truncate; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "Error: $command_name not found (install parted and mtools)" >&2
        exit 1
    }
done

mkdir -p "$(dirname "$output")"
rm -f "$output"
if [ -n "$world_image" ]; then
    truncate -s "$((world_mb + 257))M" "$output"
else
    truncate -s 256M "$output"
fi

# VirtualBox's ARM UEFI firmware discovers a conventional EFI System
# Partition more reliably than a whole-disk FAT superfloppy, particularly
# when the disk is exposed through VirtIO-SCSI.  Keep one MiB at each end for
# GPT metadata/alignment and put a FAT32 ESP in between.
parted -s "$output" mklabel gpt
parted -s "$output" mkpart ESP fat32 1MiB 255MiB
parted -s "$output" set 1 esp on
if [ -n "$world_image" ]; then
    parted -s "$output" mkpart distro ext4 256MiB "$((world_mb + 256))MiB"
    parted -s "$output" name 2 '"A20OS distro"'
    dd if="$world_image" of="$output" bs=1M seek=256 conv=sparse,notrunc status=none
fi

esp_offset=$((1024 * 1024))
esp_sectors=$(((255 - 1) * 1024 * 1024 / 512))
# Constrain the BPB to the GPT extent, excluding the backup GPT tail.
mformat -i "$output@@$esp_offset" -T "$esp_sectors" -F -v A20OS ::

# mtools maps one image per drive.  Use a temporary drive map so the complete
# FAT root (including /init, /mksh, /etc and runtime libraries) can share the
# UEFI system partition with EFI/BOOT/BOOTAA64.EFI.
# mktemp honours TMPDIR; pin it into the checkout (make exports build/tmp for
# recipes, this covers a manual run) so the scratch files land under build/,
# where `make clean` can remove them.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMPDIR="${TMPDIR:-$REPO_ROOT/build/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"

tmp_mtoolsrc=$(mktemp)
trap 'rm -f "$tmp_mtoolsrc"' EXIT HUP INT TERM
printf 'drive b: file="%s" offset=%s\n' "$output" "$esp_offset" > "$tmp_mtoolsrc"
if [ "$rootfs_image" != "-" ]; then
    printf 'drive a: file="%s"\n' "$rootfs_image" >> "$tmp_mtoolsrc"
    MTOOLSRC="$tmp_mtoolsrc" mcopy -s 'a:/*' b:/
fi
MTOOLSRC="$tmp_mtoolsrc" mmd b:/EFI b:/EFI/BOOT
MTOOLSRC="$tmp_mtoolsrc" mcopy -o "$efi_image" "b:/EFI/BOOT/$efi_name"
manifest=$(mktemp)
trap 'rm -f "$tmp_mtoolsrc" "$manifest"' EXIT HUP INT TERM
{
    # Name the format after the ESP entry actually staged, so an x86_64 disk is
    # not labelled as an ARM64 one and the manifest key matches the loader it
    # really carries.
    printf 'format=a20os-vbox-%s-gpt-v1\n' "$arch_slug"
    printf '%s_sha256=' "$loader_key"
    sha256sum "$efi_image" | awk '{print $1}'
    if [ "$rootfs_image" = "-" ]; then
        printf 'rootfs=embedded\n'
    else
        printf 'rootfs_sha256='
        sha256sum "$rootfs_image" | awk '{print $1}'
    fi
    if [ -n "$world_image" ]; then
        printf 'distro_partition_name=A20OS distro\n'
        printf 'distro_offset_bytes=268435456\n'
        printf 'distro_sha256='
        sha256sum "$world_image" | awk '{print $1}'
    fi
} > "$manifest"
MTOOLSRC="$tmp_mtoolsrc" mcopy -o "$manifest" b:/A20OS.MANIFEST
sha256sum "$output" > "$output.sha256"
echo "GPT/UEFI disk ($arch_slug) created: $output"
echo "Image checksum: $output.sha256"
