#!/usr/bin/env bash
# Build the VisionFive 2 boot firmware entirely from source:
#   OpenSBI (generic fw_dynamic) + U-Boot (SPL with sfspl header).
#
# Outputs (into $OUT_DIR, default build/vf2-firmware/):
#   u-boot-spl.bin.normal.out   SPL with StarFive header, for flash offset 0x0
#                               and the SD-card GPT "spl" partition
#   fw_dynamic.bin              OpenSBI firmware, FIT ingredient
#   jh7110-starfive-visionfive-2-v1.2a.dtb / -v1.3b.dtb   board DTBs
#   u-boot.itb                  OpenSBI+U-Boot FIT (rescue/development chain)
#   mkimage                     host tool used by make-boot-image.sh
#
# Source trees live under $WORK_DIR (default /tmp/vf2-firmware) so repeated
# runs are incremental.  Versions are pinned by commit; override with
# OPENSBI_REF / UBOOT_REF.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK_DIR="${WORK_DIR:-/tmp/vf2-firmware}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/vf2-firmware}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
# U-Boot/OpenSBI/kernel console baud.  The default 115200 is fragile over
# marginal USB-TTL wiring (dropped characters); lower it (e.g. BAUDRATE=9600)
# for a tolerant bring-up link.
BAUDRATE="${BAUDRATE:-115200}"

OPENSBI_REPO="${OPENSBI_REPO:-https://github.com/riscv-software-src/opensbi.git}"
OPENSBI_REF="${OPENSBI_REF:-337c23dd66b821ac04a0f7bea313e7e7b30ecc49}"
UBOOT_REPO="${UBOOT_REPO:-https://github.com/u-boot/u-boot.git}"
UBOOT_REF="${UBOOT_REF:-527115ef6783cec49e5610c523c124b399011361}"

clone_at() { # repo ref dir
    local repo="$1" ref="$2" dir="$3"
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$repo"
    fi
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" != "$ref" ]; then
        git -C "$dir" fetch -q --depth 1 origin "$ref"
        git -C "$dir" checkout -q FETCH_HEAD
    fi
}

mkdir -p "$WORK_DIR" "$OUT_DIR"

echo "==> OpenSBI $OPENSBI_REF"
clone_at "$OPENSBI_REPO" "$OPENSBI_REF" "$WORK_DIR/opensbi"
make -C "$WORK_DIR/opensbi" PLATFORM=generic CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS"

echo "==> U-Boot $UBOOT_REF"
clone_at "$UBOOT_REPO" "$UBOOT_REF" "$WORK_DIR/u-boot"
# U-Boot's SPL DT pruning keeps only nodes marked bootph-pre-ram.  The
# upstream VF2 DTS marks the mmc1 controller but omits its pinctrl groups,
# leaving SPL with a dangling pinctrl-0 phandle and causing ENODEV before
# the SD card can be read.  Apply the small local fix once per source tree.
UBOOT_VF2_PINCTRL="$WORK_DIR/u-boot/dts/upstream/src/riscv/starfive/jh7110-common.dtsi"
if ! grep -A2 'mmc1_pins: mmc1-0' "$UBOOT_VF2_PINCTRL" | grep -q 'bootph-pre-ram'; then
    git -C "$WORK_DIR/u-boot" apply \
        "$REPO_ROOT/tools/vf2/uboot-vf2-spl-pinctrl.patch"
fi
make -C "$WORK_DIR/u-boot" starfive_visionfive2_defconfig
# Keep OpenSBI's normal boot diagnostics visible on the board console.  The
# generic Kconfig default suppresses them (0x1), which makes a failed handoff
# indistinguishable from an SPL hang during bring-up.
( cd "$WORK_DIR/u-boot" && ./scripts/config --set-val CONFIG_SPL_OPENSBI_SCRATCH_OPTIONS 0x0 && ./scripts/config --set-val CONFIG_BAUDRATE "$BAUDRATE" && make olddefconfig )
make -C "$WORK_DIR/u-boot" CROSS_COMPILE="$CROSS_COMPILE" \
    OPENSBI="$WORK_DIR/opensbi/build/platform/generic/firmware/fw_dynamic.bin" \
    -j"$JOBS"

cp "$WORK_DIR/opensbi/build/platform/generic/firmware/fw_dynamic.bin" "$OUT_DIR/"
cp "$WORK_DIR/u-boot/spl/u-boot-spl.bin.normal.out" "$OUT_DIR/"
cp "$WORK_DIR/u-boot/u-boot.itb" "$OUT_DIR/"
cp "$WORK_DIR/u-boot/tools/mkimage" "$OUT_DIR/"
cp "$WORK_DIR/u-boot/scripts/dtc/dtc" "$OUT_DIR/"
DTS_DIR="$WORK_DIR/u-boot/dts/upstream/src/riscv/starfive"
cp "$DTS_DIR/jh7110-starfive-visionfive-2-v1.2a.dtb" "$OUT_DIR/"
cp "$DTS_DIR/jh7110-starfive-visionfive-2-v1.3b.dtb" "$OUT_DIR/"

echo "==> firmware artifacts in $OUT_DIR"
ls -la "$OUT_DIR"
