#!/usr/bin/env bash
# Build reproducible Linux kernels used only as the stage-9 BuildStorm baseline.

set -euo pipefail

usage() {
    echo "usage: $0 LINUX_SOURCE_DIR RISCV_OUTPUT_DIR LOONGARCH_OUTPUT_DIR" >&2
    exit 2
}

[[ $# -eq 3 ]] || usage
source_dir=$(readlink -f "$1")
rv_output=$(readlink -m "$2")
la_output=$(readlink -m "$3")
jobs_per_arch=${LINUX_BASELINE_BUILD_JOBS_PER_ARCH:-8}

if [[ ! "$jobs_per_arch" =~ ^[1-9][0-9]*$ ]]; then
    echo "[linux-baseline-build] jobs per architecture must be positive" >&2
    exit 2
fi
if [[ ! -r "$source_dir/Makefile" || ! -x "$source_dir/scripts/config" ]]; then
    echo "[linux-baseline-build] invalid Linux source directory: $source_dir" >&2
    exit 1
fi
for command_name in make riscv64-linux-gnu-gcc loongarch64-linux-gnu-gcc; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "[linux-baseline-build] missing command: $command_name" >&2
        exit 127
    fi
done

mkdir -p "$rv_output" "$la_output"

configure_kernel() {
    local linux_arch=$1
    local cross_compile=$2
    local output_dir=$3

    make -C "$source_dir" O="$output_dir" ARCH="$linux_arch" \
        CROSS_COMPILE="$cross_compile" defconfig
    "$source_dir/scripts/config" --file "$output_dir/.config" \
        --disable MODULES \
        --disable DEBUG_INFO \
        --disable DEBUG_INFO_BTF \
        --enable VIRTIO \
        --enable VIRTIO_BLK \
        --enable VIRTIO_MMIO \
        --enable VIRTIO_PCI \
        --enable EXT4_FS \
        --enable DEVTMPFS \
        --enable DEVTMPFS_MOUNT \
        --enable SERIAL_8250 \
        --enable SERIAL_8250_CONSOLE \
        --set-str LOCALVERSION -a20os-baseline
    if [[ "$linux_arch" == loongarch ]]; then
        "$source_dir/scripts/config" --file "$output_dir/.config" \
            --enable EFI --enable EFI_STUB --enable EFI_ZBOOT
    fi
    make -C "$source_dir" O="$output_dir" ARCH="$linux_arch" \
        CROSS_COMPILE="$cross_compile" olddefconfig
}

configure_kernel riscv riscv64-linux-gnu- "$rv_output"
configure_kernel loongarch loongarch64-linux-gnu- "$la_output"

set +e
make -C "$source_dir" O="$rv_output" ARCH=riscv \
    CROSS_COMPILE=riscv64-linux-gnu- -j"$jobs_per_arch" Image \
    >"$rv_output/build.log" 2>&1 &
rv_build_pid=$!
make -C "$source_dir" O="$la_output" ARCH=loongarch \
    CROSS_COMPILE=loongarch64-linux-gnu- -j"$jobs_per_arch" vmlinuz.efi \
    >"$la_output/build.log" 2>&1 &
la_build_pid=$!
wait "$rv_build_pid"
rv_status=$?
wait "$la_build_pid"
la_status=$?
set -e

echo "[linux-baseline-build] riscv64_status=$rv_status log=$rv_output/build.log"
echo "[linux-baseline-build] loongarch64_status=$la_status log=$la_output/build.log"
if (( rv_status != 0 || la_status != 0 )); then
    exit 1
fi

sha256sum \
    "$rv_output/arch/riscv/boot/Image" \
    "$rv_output/.config" \
    "$la_output/arch/loongarch/boot/vmlinuz.efi" \
    "$la_output/.config"
