#!/usr/bin/env bash
# Run one exclusive Linux BuildStorm baseline sample on a fresh official overlay.

set -euo pipefail

usage() {
    echo "usage: $0 riscv64|loongarch64" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage
arch=$1
case "$arch" in
riscv64)
    image_name=sdcard-rv-pub.img.gz
    expected_archive_sha=cba87f43ae569bcf2b8e4614f75cec1bf51bedb2804626fe466fcce3861df6f1
    expected_guest_arch=riscv64
    kernel_default=/tmp/linux-build-riscv64/arch/riscv/boot/Image
    config_default=/tmp/linux-build-riscv64/.config
    qemu=qemu-system-riscv64
    poweroff_cc=riscv64-linux-gnu-gcc
    root_device=/dev/vda
    control_device=/dev/vdb
    ;;
loongarch64)
    image_name=sdcard-la-pub.img.gz
    expected_archive_sha=2c411447274fbd83505d2fac505a5d9e8ed8ff3bdfc3d2d6cbdb8f61ff7d90d2
    expected_guest_arch=loongarch64
    kernel_default=/tmp/linux-build-loongarch64/arch/loongarch/boot/vmlinuz.efi
    config_default=/tmp/linux-build-loongarch64/.config
    qemu=qemu-system-loongarch64
    poweroff_cc=loongarch64-linux-gnu-gcc
    root_device=/dev/vdb
    control_device=/dev/vda
    ;;
*)
    usage
    ;;
esac

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd "$script_dir/.." && pwd -P)
cd "$repo_root"

state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}
image_dir=${FINAL_EVAL_IMAGE_DIR:-contest/2026OSImage-Pub}
timeout_s=${LINUX_BASELINE_TIMEOUT:-3000}
verify_base=${LINUX_BASELINE_VERIFY_BASE:-1}
allow_dirty=${LINUX_BASELINE_ALLOW_DIRTY:-0}
source_archive=${LINUX_BASELINE_SOURCE_ARCHIVE:-/tmp/linux-7.1.6.tar.xz}
source_archive_expected_sha=${LINUX_BASELINE_SOURCE_SHA256:-995dd7188d924662b94b48fd6fb783587267590e5b8bb33dade2c771e7d855c1}
firmware=${LINUX_BASELINE_LOONGARCH_FIRMWARE:-/usr/share/edk2/loongarch64/QEMU_EFI.fd}
conda_env=a20os

if [[ "$arch" == riscv64 ]]; then
    kernel=${LINUX_BASELINE_RISCV64_KERNEL:-$kernel_default}
    kernel_config=${LINUX_BASELINE_RISCV64_CONFIG:-$config_default}
else
    kernel=${LINUX_BASELINE_LOONGARCH64_KERNEL:-$kernel_default}
    kernel_config=${LINUX_BASELINE_LOONGARCH64_CONFIG:-$config_default}
fi

if [[ ! "$timeout_s" =~ ^[1-9][0-9]*$ ]]; then
    echo "[linux-baseline] timeout must be a positive integer" >&2
    exit 2
fi
if [[ "$verify_base" != 0 && "$verify_base" != 1 ]]; then
    echo "[linux-baseline] LINUX_BASELINE_VERIFY_BASE must be 0 or 1" >&2
    exit 2
fi
if [[ "$allow_dirty" != 0 && "$allow_dirty" != 1 ]]; then
    echo "[linux-baseline] LINUX_BASELINE_ALLOW_DIRTY must be 0 or 1" >&2
    exit 2
fi

required_commands=(
    awk cat conda cp cut flock git gzip lscpu mkfs.ext4 nproc pgrep qemu-img \
    readlink rg sed sha256sum stat tail tr truncate "$qemu" "$poweroff_cc"
)
for command_name in "${required_commands[@]}"; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "[linux-baseline] missing required command: $command_name" >&2
        exit 127
    fi
done

official_script=contest/testsuits-for-oskernel/scripts/buildstorm_testcode.sh
official_tests_repo=contest/testsuits-for-oskernel
image_gz="$image_dir/$image_name"
for required_file in "$kernel" "$kernel_config" "$source_archive" "$image_gz" \
    "$official_script" tools/run_linux_buildstorm_console.py; do
    if [[ ! -r "$required_file" ]]; then
        echo "[linux-baseline] missing required file: $required_file" >&2
        exit 1
    fi
done
if [[ "$arch" == loongarch64 && ! -r "$firmware" ]]; then
    echo "[linux-baseline] missing LoongArch EFI firmware: $firmware" >&2
    exit 1
fi
if ! conda run -n "$conda_env" python --version >/dev/null; then
    echo "[linux-baseline] conda environment '$conda_env' is unavailable" >&2
    exit 1
fi

archive_sha=$(sha256sum "$image_gz" | awk '{print $1}')
if [[ "$archive_sha" != "$expected_archive_sha" ]]; then
    echo "[linux-baseline] official archive checksum mismatch: $image_gz" >&2
    exit 1
fi
source_archive_sha=$(sha256sum "$source_archive" | awk '{print $1}')
if [[ "$source_archive_sha" != "$source_archive_expected_sha" ]]; then
    echo "[linux-baseline] Linux source archive checksum mismatch" >&2
    exit 1
fi

base_image="$state_dir/images/${arch}-${archive_sha}-official-base.img"
base_sha_file="${base_image}.sha256"
if [[ ! -r "$base_image" || ! -r "$base_sha_file" ]]; then
    echo "[linux-baseline] missing immutable official base: $base_image" >&2
    exit 1
fi
base_mode=$(stat -c '%a' "$base_image")
if [[ "$base_mode" != 444 ]]; then
    echo "[linux-baseline] official base is not mode 0444: $base_image" >&2
    exit 1
fi
base_sha=$(tr -d '[:space:]' <"$base_sha_file")
if [[ "$verify_base" == 1 ]]; then
    actual_base_sha=$(sha256sum "$base_image" | awk '{print $1}')
    if [[ "$actual_base_sha" != "$base_sha" ]]; then
        echo "[linux-baseline] official base checksum mismatch: $base_image" >&2
        exit 1
    fi
fi

mkdir -p "$state_dir/logs" "$state_dir/scores" "$state_dir/metadata" \
    "$state_dir/runs" "$state_dir/locks" "$state_dir/probes"

# Formal and performance QEMU runs are deliberately exclusive.
exec 7>"$state_dir/locks/stage9-performance-qemu.lock"
if ! flock -n 7; then
    echo "[linux-baseline] another stage-9 performance run holds the lock" >&2
    exit 1
fi
if pgrep -f '^qemu-system-(riscv64|loongarch64)( |$)' >/dev/null; then
    echo "[linux-baseline] refusing to measure while another target QEMU is running" >&2
    exit 1
fi

commit=$(git rev-parse --verify HEAD)
short_commit=$(git rev-parse --short=12 HEAD)
git_dirty=no
if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
    git_dirty=yes
fi
official_tests_commit=$(git -C "$official_tests_repo" rev-parse --verify HEAD)
official_tests_dirty=no
if [[ -n $(git -C "$official_tests_repo" status --porcelain) ]]; then
    official_tests_dirty=yes
fi
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
nonce=$(date -u +%N)
run_id="${arch}-linux-buildstorm-baseline-${short_commit}-${timestamp}-${nonce}-$$"
artifact_stem="${arch}-linux-buildstorm-baseline-${commit}-${timestamp}-${nonce}-$$"
run_dir="$state_dir/runs/$run_id"
serial_log="$state_dir/logs/${artifact_stem}.log"
score_json="$state_dir/scores/${artifact_stem}.json"
metadata="$state_dir/metadata/${artifact_stem}.txt"
mkdir "$run_dir"

control_dir="$run_dir/control-root"
control_image="$run_dir/control.img"
mkdir "$control_dir"
cp --reflink=auto "$official_script" "$control_dir/buildstorm_testcode.sh"
"$poweroff_cc" -nostdlib -static -Wl,--build-id=none \
    -o "$control_dir/a20-poweroff" tools/linux_baseline_poweroff.S
truncate -s 16M "$control_image"
mkfs.ext4 -q -F -O '^has_journal' -d "$control_dir" "$control_image"
chmod 0444 "$control_image"
official_script_sha=$(sha256sum "$official_script" | awk '{print $1}')
control_script_sha=$(sha256sum "$control_dir/buildstorm_testcode.sh" | awk '{print $1}')
poweroff_helper_sha=$(sha256sum "$control_dir/a20-poweroff" | awk '{print $1}')
if [[ "$official_script_sha" != "$control_script_sha" ]]; then
    echo "[linux-baseline] control script differs from the official script" >&2
    exit 1
fi

overlay="$run_dir/official-rootfs.qcow2"
base_image_abs=$(readlink -f "$base_image")
qemu-img create -q -f qcow2 -F raw -b "$base_image_abs" "$overlay"
cp --reflink=auto "$kernel_config" "$run_dir/linux.config"

guest_command='mount -t proc proc /proc 2>/dev/null; mount -t sysfs sysfs /sys 2>/dev/null; mount -t devtmpfs devtmpfs /dev 2>/dev/null; mkdir -p /a20-control; mount -t ext4 -o ro __CONTROL_DEVICE__ /a20-control; control_status=$?; echo A20_LINUX_BASELINE_CONTROL mount_status=$control_status; echo A20_LINUX_BASELINE_BOOT arch=$(uname -m) cores=$(nproc) uptime=$(cut -d" " -f1 /proc/uptime); if test -x /work/tgoskits/target/debug/tg-xtask; then helper_exec=yes; else helper_exec=no; fi; echo A20_LINUX_BASELINE_HELPER before mode=$(stat -c %a /work/tgoskits/target/debug/tg-xtask 2>/dev/null) bytes=$(stat -c %s /work/tgoskits/target/debug/tg-xtask 2>/dev/null) executable=$helper_exec; /bin/sh /a20-control/buildstorm_testcode.sh; test_status=$?; if test -x /work/tgoskits/target/debug/tg-xtask; then helper_exec=yes; else helper_exec=no; fi; echo A20_LINUX_BASELINE_HELPER after mode=$(stat -c %a /work/tgoskits/target/debug/tg-xtask 2>/dev/null) bytes=$(stat -c %s /work/tgoskits/target/debug/tg-xtask 2>/dev/null) executable=$helper_exec; echo A20_LINUX_BASELINE_DONE status=$test_status uptime=$(cut -d" " -f1 /proc/uptime); /a20-control/a20-poweroff; poweroff_status=$?; echo A20_LINUX_BASELINE_POWEROFF status=$poweroff_status'
guest_command=${guest_command/__CONTROL_DEVICE__/$control_device}

common_qemu_args=(
    -machine virt
    -accel tcg,thread=multi
    -m 8G
    -smp 8
    -nographic
    -kernel "$kernel"
    -append "root=$root_device rw rootwait rootfstype=ext4 console=ttyS0 earlycon init=/bin/sh"
)
if [[ "$arch" == riscv64 ]]; then
    qemu_command=(
        "$qemu" "${common_qemu_args[@]}"
        -bios default
        -global virtio-mmio.force-legacy=false
        -drive "file=$control_image,if=none,format=raw,readonly=on,id=x0"
        -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
        -drive "file=$overlay,if=none,format=qcow2,id=x1"
        -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
        -netdev user,id=net
        -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4
        -no-reboot
    )
else
    qemu_command=(
        "$qemu" "${common_qemu_args[@]}"
        -bios "$firmware"
        -drive "file=$control_image,if=none,format=raw,readonly=on,id=x0"
        -device virtio-blk-pci,drive=x0
        -drive "file=$overlay,if=none,format=qcow2,id=x1"
        -device virtio-blk-pci,drive=x1
        -netdev user,id=net
        -device virtio-net-pci,netdev=net
        -no-reboot
    )
fi
printf -v qemu_command_text '%q ' "${qemu_command[@]}"

start_time=$(date --iso-8601=seconds)
start_epoch=$(date +%s)
kernel_sha=$(sha256sum "$kernel" | awk '{print $1}')
kernel_config_sha=$(sha256sum "$kernel_config" | awk '{print $1}')
qemu_version=$("$qemu" --version | head -n 1)
python_version=$(conda run -n "$conda_env" python --version 2>&1)
host_governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unavailable)
host_load_start=$(cut -d' ' -f1-3 /proc/loadavg)

{
    echo "run_id=$run_id"
    echo "git_commit=$commit"
    echo "git_dirty=$git_dirty"
    echo "allow_dirty=$allow_dirty"
    echo "architecture=$arch"
    echo "baseline_kernel=Linux"
    echo "baseline_kernel_version=7.1.6-a20os-baseline"
    echo "start_time=$start_time"
    echo "conda_environment=$conda_env"
    echo "python_version=$python_version"
    echo "qemu_version=$qemu_version"
    echo "qemu_memory=8G"
    echo "qemu_smp=8"
    echo "qemu_accel=tcg,thread=multi"
    echo "qemu_timeout_s=$timeout_s"
    echo "linux_root_device=$root_device"
    echo "linux_control_device=$control_device"
    echo "qemu_command=$qemu_command_text"
    echo "linux_source_archive=$source_archive"
    echo "linux_source_archive_sha256=$source_archive_sha"
    echo "linux_kernel_path=$kernel"
    echo "linux_kernel_sha256=$kernel_sha"
    echo "linux_config_path=$run_dir/linux.config"
    echo "linux_config_sha256=$kernel_config_sha"
    if [[ "$arch" == loongarch64 ]]; then
        echo "linux_firmware=$firmware"
        echo "linux_firmware_sha256=$(sha256sum "$firmware" | awk '{print $1}')"
    fi
    echo "official_script=$official_script"
    echo "official_script_sha256=$official_script_sha"
    echo "poweroff_helper_sha256=$poweroff_helper_sha"
    echo "official_tests_commit=$official_tests_commit"
    echo "official_tests_dirty=$official_tests_dirty"
    echo "official_image_archive=$image_gz"
    echo "official_image_archive_sha256=$archive_sha"
    echo "official_image_base=$base_image"
    echo "official_image_sha256=$base_sha"
    echo "official_image_base_mode=$base_mode"
    echo "official_image_base_readonly=yes"
    echo "official_image_overlay=$overlay"
    echo "control_image=$control_image"
    echo "control_image_readonly=yes"
    echo "host_nproc=$(nproc)"
    echo "host_cpu_model=$(lscpu | awk -F: '/Model name/{sub(/^[ \t]+/, "", $2); print $2; exit}')"
    echo "host_cpu_governor_start=$host_governor"
    echo "host_load_start=$host_load_start"
} >"$metadata"

echo "[linux-baseline] run=$run_id timeout=${timeout_s}s"
set +e
conda run -n "$conda_env" --no-capture-output python \
    tools/run_linux_buildstorm_console.py \
    --timeout "$timeout_s" \
    --log "$serial_log" \
    --guest-command "$guest_command" \
    -- "${qemu_command[@]}"
qemu_status=$?
set -e

end_time=$(date --iso-8601=seconds)
end_epoch=$(date +%s)
runner_elapsed_s=$((end_epoch - start_epoch))
timed_out=no
if [[ "$qemu_status" -eq 124 ]]; then
    timed_out=yes
fi
host_governor_end=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unavailable)
host_load_end=$(cut -d' ' -f1-3 /proc/loadavg)

set +e
conda run -n "$conda_env" python \
    contest/testsuits-for-oskernel/judge/judge_buildstorm-glibc.py \
    "$serial_log" >"$score_json"
judge_status=$?
set -e

compile_line=$(rg 'BUILDSTORM_COMPILE mode=multi ' "$serial_log" | tr -d '\r' | tail -n 1 || true)
compile_elapsed_s=$(sed -nE 's/.* elapsed_s=([^ ]+).*/\1/p' <<<"$compile_line")
guest_cores=$(sed -nE 's/.* cores=([0-9]+).*/\1/p' <<<"$compile_line")
artifact_bytes=$(sed -nE 's/.* bytes=([0-9]+).*/\1/p' <<<"$compile_line")
helper_before=$(rg 'A20_LINUX_BASELINE_HELPER before ' "$serial_log" | tr -d '\r' | tail -n 1 || true)
helper_after=$(rg 'A20_LINUX_BASELINE_HELPER after ' "$serial_log" | tr -d '\r' | tail -n 1 || true)

{
    echo "end_time=$end_time"
    echo "runner_elapsed_s=$runner_elapsed_s"
    echo "qemu_exit_status=$qemu_status"
    echo "qemu_timed_out=$timed_out"
    echo "judge_exit_status=$judge_status"
    echo "serial_log=$serial_log"
    echo "score_json=$score_json"
    echo "guest_cores=$guest_cores"
    echo "compile_elapsed_s=$compile_elapsed_s"
    echo "artifact_bytes=$artifact_bytes"
    echo "helper_before=$helper_before"
    echo "helper_after=$helper_after"
    echo "host_cpu_governor_end=$host_governor_end"
    echo "host_load_end=$host_load_end"
} >>"$metadata"

pass=yes
if [[ "$allow_dirty" == 0 && "$git_dirty" != no ]] || \
   [[ "$official_tests_dirty" != no || "$qemu_status" -ne 0 || \
      "$timed_out" != no || "$judge_status" -ne 0 ]]; then
    pass=no
fi
if ! rg -q '^A20_LINUX_BASELINE_CONTROL mount_status=0\r*$' "$serial_log" || \
   ! rg -q "^A20_LINUX_BASELINE_BOOT arch=${expected_guest_arch} cores=8 " "$serial_log" || \
   ! rg -q '^A20_LINUX_BASELINE_DONE status=0 ' "$serial_log" || \
   ! rg -q '^BUILDSTORM_TOOLCHAIN ok\r*$' "$serial_log" || \
   ! rg -q '^BUILDSTORM_MINIBUILD ok\r*$' "$serial_log" || \
   ! rg -q '^BUILDSTORM_COMPILE mode=multi ok=true ' "$serial_log" || \
   ! rg -q '^A20_LINUX_BASELINE_HELPER before .* executable=yes\r*$' "$serial_log" || \
   ! rg -q '^A20_LINUX_BASELINE_HELPER after .* executable=yes\r*$' "$serial_log"; then
    pass=no
fi
if rg -q 'Kernel panic|Out of memory|BUG:|A20_LINUX_BASELINE_DRIVER (timeout|shutdown_timeout)=true|A20_LINUX_BASELINE_POWEROFF status=[1-9]' \
    "$serial_log"; then
    pass=no
fi
if [[ "$guest_cores" != 8 || ! "$artifact_bytes" =~ ^[0-9]+$ || \
      "$artifact_bytes" -lt 500000 || -z "$compile_elapsed_s" ]]; then
    pass=no
fi
if ! awk -v elapsed="$compile_elapsed_s" 'BEGIN { exit !(elapsed + 0 > 0) }'; then
    pass=no
fi
if [[ "$runner_elapsed_s" -gt "$timeout_s" ]]; then
    pass=no
fi

echo "linux_baseline_pass=$pass" >>"$metadata"
if [[ "$pass" != yes ]]; then
    echo "[linux-baseline] FAIL arch=$arch log=$serial_log metadata=$metadata" >&2
    exit 1
fi

echo "[linux-baseline] PASS arch=$arch compile_elapsed_s=$compile_elapsed_s runner_elapsed_s=$runner_elapsed_s bytes=$artifact_bytes"
echo "[linux-baseline] log=$serial_log"
echo "[linux-baseline] score=$score_json"
echo "[linux-baseline] metadata=$metadata"
