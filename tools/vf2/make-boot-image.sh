#!/usr/bin/env bash
# Assemble VisionFive 2 boot artifacts from a built A20OS kernel and the
# from-source firmware (tools/vf2/build-firmware.sh):
#
#   a20os.itb      FIT: OpenSBI (firmware) + A20OS kernel (loadable) + DTBs.
#                  Boot chain: BootROM -> SPL -> OpenSBI -> A20OS.
#   a20os-sd.img   Raw SD-card image: GPT spl partition @2 MiB, FIT partition
#                  @4 MiB, optional FAT32 userspace partition @32 MiB, and an
#                  optional ext4 extra-packages partition after it.
#
# Usage:
#   tools/vf2/make-boot-image.sh [path/to/kernel.bin] [path/to/fat32.img]
#                              [path/to/extra.ext4.img]
#
# The same a20os.itb goes to QSPI flash offset 0x100000 for flash boot
# (see docs/platforms/visionfive2-boot.md).
set -euo pipefail

# Administrative disk utilities commonly live outside an unprivileged
# user's PATH even though they are installed on the build host.
PATH="/usr/sbin:/sbin:$PATH"
export PATH

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FW_DIR="${FW_DIR:-$REPO_ROOT/build/vf2-firmware}"
KERNEL_BIN="${1:-$REPO_ROOT/.kernel-build/riscv64-visionfive2-linux-dev-nommu/kernel.bin}"
ROOTFS_IMG="${2:-${VF2_ROOTFS_IMAGE:-}}"
EXTRA_IMG="${3:-${VF2_EXTRA_IMAGE:-}}"
SD_IMAGE_SIZE_MB="${SD_IMAGE_SIZE_MB:-32}"

# JH7110 SD/eMMC GPT layout (include/configs/starfive-visionfive2.h)
SPL_PART_GUID="2E54B353-1271-4842-806F-E436D6AF6985"
FIT_PART_GUID="BC13C2FF-59E6-4262-A352-B275FD6F7172"
SPL_PART_SECTOR=4096   # 2 MiB
FIT_PART_SECTOR=8192   # 4 MiB
ROOTFS_PART_SECTOR=65536 # 32 MiB, after the boot FIT area
EXTRA_PART_SECTOR=""

[ -f "$KERNEL_BIN" ] || { echo "missing kernel: $KERNEL_BIN" >&2; exit 1; }
[ -f "$FW_DIR/fw_dynamic.bin" ] || {
    echo "missing firmware in $FW_DIR; run tools/vf2/build-firmware.sh first" >&2
    exit 1; }

if [ -n "$ROOTFS_IMG" ]; then
    [ -f "$ROOTFS_IMG" ] || { echo "missing rootfs: $ROOTFS_IMG" >&2; exit 1; }
    rootfs_bytes=$(stat -c '%s' "$ROOTFS_IMG")
    rootfs_mb=$(( (rootfs_bytes + 1048575) / 1048576 ))
    # Leave space for GPT's backup header and partition-entry array.
    rootfs_end_mb=$((ROOTFS_PART_SECTOR / 2048 + rootfs_mb + 1))
    if [ "$SD_IMAGE_SIZE_MB" -lt "$rootfs_end_mb" ]; then
        SD_IMAGE_SIZE_MB="$rootfs_end_mb"
    fi
fi

if [ -n "$EXTRA_IMG" ]; then
    [ -f "$EXTRA_IMG" ] || { echo "missing extra image: $EXTRA_IMG" >&2; exit 1; }
    extra_bytes=$(stat -c '%s' "$EXTRA_IMG")
    extra_mb=$(( (extra_bytes + 1048575) / 1048576 ))
    # Keep the extra filesystem on a 1 MiB boundary and leave one MiB between
    # filesystems.  The latter makes it safe to grow the FAT32 rootfs later.
    if [ -n "$ROOTFS_IMG" ]; then
        EXTRA_PART_SECTOR=$(( (ROOTFS_PART_SECTOR + rootfs_mb * 2048 + 2047) / 2048 * 2048 + 2048 ))
    else
        EXTRA_PART_SECTOR=327680 # 160 MiB, matching the historical VF2 layout
    fi
    extra_end_mb=$((EXTRA_PART_SECTOR / 2048 + extra_mb + 1))
    if [ "$SD_IMAGE_SIZE_MB" -lt "$extra_end_mb" ]; then
        SD_IMAGE_SIZE_MB="$extra_end_mb"
    fi
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp "$KERNEL_BIN" "$STAGE/kernel.bin"
cp "$FW_DIR/fw_dynamic.bin" "$STAGE/"
cp "$FW_DIR/jh7110-starfive-visionfive-2-v1.2a.dtb" "$STAGE/"
cp "$FW_DIR/jh7110-starfive-visionfive-2-v1.3b.dtb" "$STAGE/"
cp "$REPO_ROOT/tools/vf2/a20os-fit.its" "$STAGE/"

# mkimage needs dtc for /incbin/; use the ones built with U-Boot.
# NOTE: keep image data inline (no -E).  Binman's u-boot.itb is inline too,
# and the SPL external-data (data-offset) path is the one thing that differs
# from the proven-working upstream boot flow on JH7110.
DTC="$FW_DIR/dtc"
[ -x "$DTC" ] || { echo "missing dtc: $DTC" >&2; exit 1; }

( cd "$STAGE" && PATH="$(dirname "$DTC"):$PATH" \
    "$FW_DIR/mkimage" -f a20os-fit.its a20os.itb )

cp "$STAGE/a20os.itb" "$FW_DIR/a20os.itb"
echo "==> FIT: $FW_DIR/a20os.itb"
"$FW_DIR/mkimage" -l "$FW_DIR/a20os.itb"

# ---- raw SD card image ------------------------------------------------
IMG="$FW_DIR/a20os-sd.img"
dd if=/dev/zero of="$IMG" bs=1M count="$SD_IMAGE_SIZE_MB" status=none
sgdisk -Z "$IMG" >/dev/null
sgdisk -n "1:$SPL_PART_SECTOR:+1M" -t "1:$SPL_PART_GUID" -c 1:spl "$IMG" >/dev/null
sgdisk -n "2:$FIT_PART_SECTOR:+12M" -t "2:$FIT_PART_GUID" -c 2:uboot "$IMG" >/dev/null
if [ -n "$ROOTFS_IMG" ]; then
    sgdisk -n "3:$ROOTFS_PART_SECTOR:+${rootfs_mb}M" \
        -t "3:0700" -c 3:a20os-rootfs "$IMG" >/dev/null
fi
if [ -n "$EXTRA_IMG" ]; then
    sgdisk -n "4:$EXTRA_PART_SECTOR:+${extra_mb}M" \
        -t "4:8300" -c 4:a20os-extra "$IMG" >/dev/null
fi
dd if="$FW_DIR/u-boot-spl.bin.normal.out" of="$IMG" bs=512 \
    seek="$SPL_PART_SECTOR" conv=notrunc status=none
dd if="$FW_DIR/a20os.itb" of="$IMG" bs=512 \
    seek="$FIT_PART_SECTOR" conv=notrunc status=none
if [ -n "$ROOTFS_IMG" ]; then
    dd if="$ROOTFS_IMG" of="$IMG" bs=512 \
        seek="$ROOTFS_PART_SECTOR" conv=notrunc status=none
fi
if [ -n "$EXTRA_IMG" ]; then
    dd if="$EXTRA_IMG" of="$IMG" bs=512 \
        seek="$EXTRA_PART_SECTOR" conv=notrunc status=none
fi
echo "==> SD image: $IMG (write with: dd if=$IMG of=/dev/sdX bs=4M conv=fsync)"
sgdisk -p "$IMG"
