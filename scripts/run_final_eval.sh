#!/usr/bin/env bash
#
# Reproducible 2026 final-round evaluation entry point.
#
# Usage:
#   scripts/run_final_eval.sh riscv64|loongarch64 cagent|buildstorm
#   scripts/run_final_eval.sh riscv64|loongarch64 buildstorm-probe 1|8 [probe-case]
#
# The official ext4 image is never opened writable.  A read-only base restored
# from the published .gz and a fresh qcow2 overlay are used for every run.

set -euo pipefail

usage() {
    echo "usage: $0 riscv64|loongarch64 cagent|buildstorm" >&2
    echo "       $0 riscv64|loongarch64 buildstorm-probe 1|8 [probe-case]" >&2
    exit 2
}

[[ $# -ge 2 && $# -le 4 ]] || usage
arch=$1
group=$2
guest_cpus=${3:-8}
probe_case=${4:-}

case "$arch" in
riscv64)
    arch_tag=riscv64
    image_name=sdcard-rv-pub.img.gz
    qemu=qemu-system-riscv64
    ;;
loongarch64)
    arch_tag=loongarch64
    image_name=sdcard-la-pub.img.gz
    qemu=qemu-system-loongarch64
    ;;
*)
    usage
    ;;
esac

case "$group" in
cagent)
    default_timeout=900
    judge_name=judge_cagent-glibc.py
    ;;
buildstorm)
    default_timeout=36000
    judge_name=judge_buildstorm-glibc.py
    ;;
buildstorm-probe)
    default_timeout=1800
    judge_name=
    [[ $# -ge 3 && $# -le 4 ]] || usage
    ;;
*)
    usage
    ;;
esac
if [[ -n "$probe_case" && "$group" != buildstorm-probe ]]; then
    usage
fi
if [[ -n "$probe_case" && ! "$probe_case" =~ ^[a-z0-9][a-z0-9-]*$ ]]; then
    echo "[final-eval] invalid probe case: $probe_case" >&2
    exit 2
fi
if [[ "$guest_cpus" != 1 && "$guest_cpus" != 8 ]]; then
    echo "[final-eval] guest CPU count must be 1 or 8" >&2
    exit 2
fi
if [[ "$group" != buildstorm-probe && "$guest_cpus" != 8 ]]; then
    echo "[final-eval] formal cagent/buildstorm runs require 8 guest CPUs" >&2
    exit 2
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd "$script_dir/.." && pwd -P)
cd "$repo_root"

image_dir=${FINAL_EVAL_IMAGE_DIR:-contest/2026OSImage-Pub}
state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}
timeout_s=${FINAL_EVAL_TIMEOUT:-$default_timeout}
verify_base=${FINAL_EVAL_VERIFY_BASE:-1}
conda_env=a20os
image_gz="$image_dir/$image_name"
judge="contest/testsuits-for-oskernel/judge/$judge_name"

if [[ ! "$timeout_s" =~ ^[1-9][0-9]*$ ]]; then
    echo "[final-eval] FINAL_EVAL_TIMEOUT must be a positive integer (seconds)" >&2
    exit 2
fi
if [[ "$verify_base" != 0 && "$verify_base" != 1 ]]; then
    echo "[final-eval] FINAL_EVAL_VERIFY_BASE must be 0 or 1" >&2
    exit 2
fi

required_commands=(
    conda dd flock gzip mcopy mdel mtype qemu-img readlink sha256sum tee "$qemu"
)
for command_name in "${required_commands[@]}"; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "[final-eval] missing required command: $command_name" >&2
        exit 127
    fi
done
if [[ ! -r "$image_gz" ]]; then
    echo "[final-eval] missing official image archive: $image_gz" >&2
    exit 1
fi
if [[ -n "$judge_name" && ! -r "$judge" ]]; then
    echo "[final-eval] missing official judge: $judge" >&2
    exit 1
fi
if ! conda run -n "$conda_env" python --version >/dev/null; then
    echo "[final-eval] conda environment '$conda_env' is unavailable" >&2
    exit 1
fi

mkdir -p \
    "$state_dir/images" \
    "$state_dir/logs" \
    "$state_dir/scores" \
    "$state_dir/metadata" \
    "$state_dir/probes" \
    "$state_dir/runs"

commit=$(git rev-parse --verify HEAD)
short_commit=$(git rev-parse --short=12 HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
run_nonce=$(date -u +%N)
if [[ "$group" == buildstorm-probe ]]; then
    probe_suffix=
    if [[ -n "$probe_case" ]]; then
        probe_suffix="-${probe_case}"
    fi
    run_id="${arch_tag}-${group}-${guest_cpus}c${probe_suffix}-${short_commit}-${timestamp}-${run_nonce}-$$"
    artifact_stem="${arch_tag}-${group}-${guest_cpus}c${probe_suffix}-${commit}-${timestamp}-${run_nonce}-$$"
else
    run_id="${arch_tag}-${group}-${short_commit}-${timestamp}-${run_nonce}-$$"
    artifact_stem="${arch_tag}-${group}-${commit}-${timestamp}-${run_nonce}-$$"
fi
run_dir="$state_dir/runs/$run_id"
mkdir "$run_dir"

serial_log="$state_dir/logs/${artifact_stem}.log"
score_json="$state_dir/scores/${artifact_stem}.json"
metadata="$state_dir/metadata/${artifact_stem}.txt"
judge_stderr="$run_dir/judge.stderr"
probe_artifacts="$state_dir/probes/$run_id"

image_gz_sha=$(sha256sum "$image_gz" | awk '{print $1}')
# Key the base cache by the published archive checksum.  A future archive
# update therefore gets a new immutable backing file and cannot silently
# change the backing of an older run's overlay.
base_image="$state_dir/images/${arch_tag}-${image_gz_sha}-official-base.img"
base_sha_file="${base_image}.sha256"
base_lock="${base_image}.lock"

# Serialise cache restoration so concurrent formal targets cannot publish a
# partial base.  dd conv=sparse avoids allocating the image's long zero runs.
exec 9>"$base_lock"
flock 9
if [[ ! -f "$base_image" || ! -f "$base_sha_file" ]]; then
    base_tmp="${base_image}.tmp.$$"
    sha_tmp="${base_sha_file}.tmp.$$"
    trap 'rm -f "${base_tmp:-}" "${sha_tmp:-}"' EXIT
    echo "[final-eval] restoring clean $arch base image from $image_gz"
    gzip -dc "$image_gz" | dd of="$base_tmp" bs=8M conv=sparse status=none
    sha256sum "$base_tmp" | awk '{print $1}' >"$sha_tmp"
    chmod 0444 "$base_tmp"
    mv "$base_tmp" "$base_image"
    mv "$sha_tmp" "$base_sha_file"
    trap - EXIT
fi
if [[ "$verify_base" == 1 ]]; then
    expected_base_sha=$(tr -d '[:space:]' <"$base_sha_file")
    actual_base_sha=$(sha256sum "$base_image" | awk '{print $1}')
    if [[ "$actual_base_sha" != "$expected_base_sha" ]]; then
        echo "[final-eval] cached base image checksum mismatch: $base_image" >&2
        exit 1
    fi
fi
base_sha=$(tr -d '[:space:]' <"$base_sha_file")
flock -u 9

build_args=(
    ARCH="$arch"
    BOARD="qemu-virt-$arch"
    ABI=both
    MODE=release
    PROFILE=full
    NR_CPUS="$guest_cpus"
    FAT32_IMAGE_MB=128
    PYTHON="conda run -n $conda_env python"
    dev-build
)
echo "[final-eval] building $arch kernel and FAT32 user image"
make "${build_args[@]}"

if [[ "$guest_cpus" == 1 ]]; then
    build_dir=".kernel-build/${arch}-qemu-virt-${arch}-both-dev"
else
    build_dir=".kernel-build/${arch}-qemu-virt-${arch}-both-dev-smp${guest_cpus}"
fi
kernel="$build_dir/kernel.elf"
fat32="$build_dir/fat32.img"
if [[ ! -s "$kernel" || ! -s "$fat32" ]]; then
    echo "[final-eval] build did not produce $kernel and $fat32" >&2
    exit 1
fi

# Keep the build artifact marker-free and make the selected final group local
# to this run.  In particular, never carry contest-mode into a final run.
run_fat32="$run_dir/fat32.img"
cp --reflink=auto "$fat32" "$run_fat32"
mdel -i "$run_fat32" ::/etc/contest-mode >/dev/null 2>&1 || true
mdel -i "$run_fat32" ::/etc/final-eval-group >/dev/null 2>&1 || true
mdel -i "$run_fat32" ::/etc/final-eval-probe-case >/dev/null 2>&1 || true
printf '%s\n' "$group" | mcopy -o -i "$run_fat32" - ::/etc/final-eval-group
if [[ -n "$probe_case" ]]; then
    printf '%s\n' "$probe_case" |
        mcopy -o -i "$run_fat32" - ::/etc/final-eval-probe-case
fi
if mtype -i "$run_fat32" ::/etc/contest-mode >/dev/null 2>&1; then
    echo "[final-eval] refusing to run with both contest entry markers" >&2
    exit 1
fi
marker=$(mtype -i "$run_fat32" ::/etc/final-eval-group | tr -d '\r\n')
if [[ "$marker" != "$group" ]]; then
    echo "[final-eval] failed to install final-eval-group=$group" >&2
    exit 1
fi

if [[ "$group" == buildstorm-probe ]]; then
    host_probe_dir="$run_dir/host-probe-build"
    bash scripts/build_buildstorm_probes.sh \
        "$arch" "$base_image" "$host_probe_dir"
    mmd -i "$run_fat32" ::/a20-probe >/dev/null 2>&1 || true
    mcopy -o -i "$run_fat32" "$host_probe_dir/cwd-probe" \
        ::/a20-probe/cwd-probe
    mcopy -o -i "$run_fat32" "$host_probe_dir/exec-pages-probe" \
        ::/a20-probe/exec-pages-probe
    mcopy -o -i "$run_fat32" "$host_probe_dir/liba20probe.so" \
        ::/a20-probe/liba20probe.so
    for probe_name in cwd-probe exec-pages-probe liba20probe.so; do
        if ! mtype -i "$run_fat32" "::/a20-probe/$probe_name" >/dev/null; then
            echo "[final-eval] failed to install $probe_name" >&2
            exit 1
        fi
    done
fi

overlay="$run_dir/official-rootfs.qcow2"
base_image_abs=$(readlink -f "$base_image")
qemu-img create -q -f qcow2 -F raw -b "$base_image_abs" "$overlay"

common_qemu_args=(
    -machine virt
    -accel tcg,thread=multi
    -m 8G
    -smp "$guest_cpus"
    -nographic
    -kernel "$kernel"
)
if [[ "$arch" == riscv64 ]]; then
    qemu_args=(
        "${common_qemu_args[@]}"
        -bios default
        -global virtio-mmio.force-legacy=false
        -drive "file=$run_fat32,if=none,format=raw,id=x0"
        -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
        -drive "file=$overlay,if=none,format=qcow2,id=x1"
        -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
        -netdev user,id=net
        -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4
        -no-reboot
    )
else
    qemu_args=(
        "${common_qemu_args[@]}"
        -drive "file=$run_fat32,if=none,format=raw,id=x0"
        -device virtio-blk-pci,drive=x0
        -drive "file=$overlay,if=none,format=qcow2,id=x1"
        -device virtio-blk-pci,drive=x1
        -netdev user,id=net
        -device virtio-net-pci,netdev=net
        -no-reboot
    )
fi
qemu_command=("$qemu" "${qemu_args[@]}")
printf -v qemu_command_text '%q ' "${qemu_command[@]}"

start_time=$(date --iso-8601=seconds)
python_version=$(conda run -n "$conda_env" python --version 2>&1)
qemu_version=$("$qemu" --version | head -n 1)
kernel_sha=$(sha256sum "$kernel" | awk '{print $1}')
fat32_sha=$(sha256sum "$run_fat32" | awk '{print $1}')
git_dirty=no
if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
    git_dirty=yes
fi

{
    echo "run_id=$run_id"
    echo "git_commit=$commit"
    echo "git_dirty=$git_dirty"
    echo "architecture=$arch"
    echo "group=$group"
    echo "start_time=$start_time"
    echo "conda_environment=$conda_env"
    echo "python_version=$python_version"
    echo "qemu_version=$qemu_version"
    echo "kernel_build_args=${build_args[*]}"
    echo "kernel_path=$kernel"
    echo "kernel_sha256=$kernel_sha"
    echo "fat32_path=$run_fat32"
    echo "fat32_sha256=$fat32_sha"
    echo "entry_marker=final-eval-group=$group"
    echo "contest_mode_present=no"
    echo "official_image_archive=$image_gz"
    echo "official_image_archive_sha256=$image_gz_sha"
    echo "official_image_base=$base_image"
    echo "official_image_sha256=$base_sha"
    echo "official_image_overlay=$overlay"
    echo "qemu_memory=8G"
    echo "qemu_smp=$guest_cpus"
    echo "qemu_accel=tcg,thread=multi"
    echo "qemu_timeout_s=$timeout_s"
    echo "qemu_command=$qemu_command_text"
    if [[ "$group" == buildstorm-probe ]]; then
        echo "probe_case=${probe_case:-all}"
        echo "probe_cwd_sha256=$(sha256sum "$host_probe_dir/cwd-probe" | awk '{print $1}')"
        echo "probe_exec_pages_sha256=$(sha256sum "$host_probe_dir/exec-pages-probe" | awk '{print $1}')"
        echo "probe_dso_sha256=$(sha256sum "$host_probe_dir/liba20probe.so" | awk '{print $1}')"
        echo "probe_process_models=static-elf,single-process,cargo-j1,cargo-default"
    fi
} >"$metadata"

echo "[final-eval] run=$run_id timeout=${timeout_s}s"
set +e
conda run -n "$conda_env" --no-capture-output \
    python scripts/run_with_timeout.py --foreground "$timeout_s" \
    "${qemu_command[@]}" 2>&1 | tee "$serial_log"
qemu_status=${PIPESTATUS[0]}
set -e

timed_out=no
if [[ "$qemu_status" -eq 124 ]]; then
    timed_out=yes
fi
end_time=$(date --iso-8601=seconds)
guest_cores=$(
    sed -nE 's/.*\[SMP\] ([0-9]+\/[0-9]+) configured CPUs online.*/\1/p' \
        "$serial_log" | tail -n 1
)
if [[ -n "$guest_cores" ]]; then
    guest_cores_source=kernel-smp-line
elif [[ "$guest_cpus" == 1 ]]; then
    guest_cores=1/1
    guest_cores_source=single-core-kernel-build-and-qemu-command
else
    guest_cores=not-observed
    guest_cores_source=not-observed
fi

set +e
if [[ "$group" == cagent ]]; then
    conda run -n "$conda_env" --no-capture-output python "$judge" \
        <"$serial_log" >"$score_json" 2>"$judge_stderr"
    judge_status=$?
elif [[ "$group" == buildstorm ]]; then
    conda run -n "$conda_env" python "$judge" "$serial_log" \
        >"$score_json" 2>"$judge_stderr"
    judge_status=$?
else
    mkdir -p "$probe_artifacts"
    awk -v output_dir="$probe_artifacts" '
        /^#### BUILDSTORM PROBE START / {
            name = $5
            output = output_dir "/" name ".log"
            active = 1
        }
        active { print >output }
        /^#### BUILDSTORM PROBE END / {
            close(output)
            active = 0
        }
    ' "$serial_log"
    grep '^\[BUILDSTORM-PROBE\] case=.* rc=' "$serial_log" \
        >"$probe_artifacts/results.txt"
    grep '^\[BUILDSTORM-PROBE\] summary ' "$serial_log" \
        >>"$probe_artifacts/results.txt"
    judge_status=0
fi
set -e
if [[ "$group" != buildstorm-probe && "$judge_status" -eq 0 ]]; then
    set +e
    conda run -n "$conda_env" python -m json.tool \
        "$score_json" >/dev/null 2>>"$judge_stderr"
    json_status=$?
    set -e
    if [[ "$json_status" -ne 0 ]]; then
        judge_status=$json_status
    fi
fi

{
    echo "end_time=$end_time"
    echo "qemu_exit_status=$qemu_status"
    echo "qemu_timed_out=$timed_out"
    echo "guest_cores=$guest_cores"
    echo "guest_cores_source=$guest_cores_source"
    if [[ "$group" == buildstorm-probe ]]; then
        echo "probe_phase=1"
        echo "probe_artifacts=$probe_artifacts"
        echo "probe_results=$probe_artifacts/results.txt"
        echo "judge=not-run"
        echo "judge_exit_status=not-run"
    else
        echo "judge=$judge"
        echo "judge_exit_status=$judge_status"
    fi
    echo "serial_log=$serial_log"
    echo "score_json=$score_json"
    echo "judge_stderr=$judge_stderr"
} >>"$metadata"

if [[ "$group" != buildstorm-probe && "$judge_status" -ne 0 ]]; then
    echo "[final-eval] judge failed with status $judge_status" >&2
    exit "$judge_status"
fi

echo "[final-eval] serial:   $serial_log"
if [[ "$group" == buildstorm-probe ]]; then
    echo "[final-eval] probes:  $probe_artifacts"
else
    echo "[final-eval] score:    $score_json"
fi
echo "[final-eval] metadata: $metadata"
if [[ "$qemu_status" -ne 0 ]]; then
    echo "[final-eval] QEMU ended with status $qemu_status (timed_out=$timed_out)" >&2
    exit "$qemu_status"
fi
