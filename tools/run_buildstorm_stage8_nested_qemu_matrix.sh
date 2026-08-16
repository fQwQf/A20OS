#!/usr/bin/env bash
#
# Exercise the online evaluator's nested-QEMU boundary.  Each architecture
# gets consecutive cold A20 boots backed by independent qcow2 overlays.  The
# RISC-V lane boots the generated ArceOS artifact; the LoongArch lane executes
# a direct TCG payload with the official guest RAM size, independently of the
# tg-xtask UEFI/FAT preparation layer.

set -euo pipefail

rounds=${1:-2}
state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}
budget_s=${BUILDSTORM_MATRIX_BUDGET_SECONDS:-3600}

if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-round-count]" >&2
    exit 2
fi
if [[ ! "$budget_s" =~ ^[1-9][0-9]*$ ]]; then
    echo "BUILDSTORM_MATRIX_BUDGET_SECONDS must be positive" >&2
    exit 2
fi

commit=$(git rev-parse --verify HEAD)
official_tests_commit=$(git -C contest/testsuits-for-oskernel rev-parse --verify HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
summary=${BUILDSTORM_MATRIX_SUMMARY:-"$state_dir/probes/stage8-nested-qemu-matrix-${commit}-${timestamp}.txt"}
mkdir -p "$state_dir/probes"

if [[ ${BUILDSTORM_MATRIX_INTERNAL:-0} != 1 ]]; then
    export BUILDSTORM_MATRIX_INTERNAL=1
    export BUILDSTORM_MATRIX_SUMMARY="$summary"
    set +e
    timeout --signal=TERM --kill-after=30 "$budget_s" bash "$0" "$rounds"
    status=$?
    set -e
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX TIME_BUDGET_EXCEEDED budget_s=$budget_s" |
            tee -a "$summary" >&2
    fi
    exit "$status"
fi

exec > >(tee "$summary") 2>&1
matrix_work_dir=$(mktemp -d /tmp/a20os-buildstorm-stage8-nested-qemu.XXXXXX)
export FINAL_EVAL_IMAGE_CACHE_DIR=${FINAL_EVAL_IMAGE_CACHE_DIR:-/tmp/a20os-buildstorm-matrix-cache}
export FINAL_EVAL_WORK_DIR="$matrix_work_dir"
cleanup() {
    rm -rf -- "$matrix_work_dir"
}
trap cleanup EXIT

echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX commit=$commit rounds=$rounds budget_s=$budget_s"
echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX official_tests_commit=$official_tests_commit"
echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX cache=$FINAL_EVAL_IMAGE_CACHE_DIR work=$FINAL_EVAL_WORK_DIR"
started_at=$(date +%s)

metadata_value() {
    local key=$1
    local metadata=$2
    sed -n "s/^${key}=//p" "$metadata" | tail -n 1
}

run_lane() {
    local arch=$1
    local target expected_hash expected_binary expected_artifact
    local round log metadata overlay boot_marker boot_marker_seen boot_rc boot_output

    case "$arch" in
    riscv64)
        target=final-stage8-rv-nested-qemu
        expected_hash=cba87f43ae569bcf2b8e4614f75cec1bf51bedb2804626fe466fcce3861df6f1
        expected_binary=/opt/qemu-rv64/bin/qemu-system-riscv64
        expected_artifact=/work/tgoskits/target/riscv64gc-unknown-linux-musl/release/arceos-helloworld.bin
        ;;
    loongarch64)
        target=final-stage8-la-nested-qemu
        expected_hash=2c411447274fbd83505d2fac505a5d9e8ed8ff3bdfc3d2d6cbdb8f61ff7d90d2
        expected_binary=/opt/qemu-la64/bin/qemu-system-loongarch64
        expected_artifact=/work/tgoskits/target/loongarch64-unknown-linux-musl/release/arceos-helloworld.bin
        ;;
    esac

    for ((round = 1; round <= rounds; round++)); do
        echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX start arch=$arch round=$round/$rounds"
        make "$target"
        log=$(ls -1t \
            "$state_dir"/logs/"${arch}-buildstorm-probe-8c-stage8-nested-qemu-"*.log |
            head -n 1)
        metadata=${log/\/logs\//\/metadata\/}
        metadata=${metadata%.log}.txt

        rg -q '^BUILDSTORM_STAGE8_NESTED_QEMU ok$' "$log"
        rg -q '^\[BUILDSTORM-PROBE\] summary total=1 failures=0$' "$log"
        if rg -q 'stack smashing detected|SIGILL:|Kernel panic|unhandled exception|lifetime_errors: [1-9]' "$log"; then
            echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX unexpected-fault log=$log" >&2
            return 1
        fi

        [[ $(metadata_value git_commit "$metadata") == "$commit" ]]
        [[ $(metadata_value git_dirty "$metadata") == no ]]
        [[ $(metadata_value official_tests_commit "$metadata") == "$official_tests_commit" ]]
        [[ $(metadata_value official_tests_dirty "$metadata") == no ]]
        [[ $(metadata_value official_image_archive_sha256 "$metadata") == "$expected_hash" ]]
        [[ $(metadata_value official_image_base_mode "$metadata") == 444 ]]
        [[ $(metadata_value official_image_base_readonly "$metadata") == yes ]]
        [[ $(metadata_value qemu_exit_status "$metadata") == 0 ]]
        [[ $(metadata_value qemu_timed_out "$metadata") == no ]]
        [[ $(metadata_value guest_cores "$metadata") == 8/8 ]]
        [[ $(metadata_value nested_qemu_version_rc "$metadata") == 0 ]]
        [[ $(metadata_value nested_qemu_binary "$metadata") == "$expected_binary" ]]
        [[ $(metadata_value nested_qemu_artifact "$metadata") == "$expected_artifact" ]]

        boot_marker=$(metadata_value nested_qemu_boot_marker "$metadata")
        if [[ "$arch" == riscv64 ]]; then
            [[ "$boot_marker" == hello_world ]]
        else
            [[ "$boot_marker" == loongarch_tcg_smoke ]]
        fi
        boot_marker_seen=$(metadata_value nested_qemu_boot_marker_seen "$metadata")
        [[ "$boot_marker_seen" == 1 ]]
        boot_rc=$(metadata_value nested_qemu_boot_rc "$metadata")
        [[ "$boot_rc" == 0 || "$boot_rc" == 124 || "$boot_rc" == 143 ]]
        boot_output=$(metadata_value nested_qemu_boot_output "$metadata")
        [[ "$boot_output" == /tmp/a20-stage8-nested-qemu-boot.out ]]

        overlay=$(metadata_value official_image_overlay "$metadata")
        [[ -n "$overlay" ]]
        echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX pass arch=$arch round=$round/$rounds marker=$boot_marker inner_rc=$boot_rc overlay=$overlay log=$log"
    done
}

pids=()
for arch in riscv64 loongarch64; do
    run_lane "$arch" &
    pids+=("$!")
done
failures=0
for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
        failures=$((failures + 1))
    fi
done
if (( failures != 0 )); then
    echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX FAIL failed_lanes=$failures" >&2
    exit 1
fi

pass_count=$(rg -c '^BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX pass ' "$summary")
unique_overlays=$(rg '^BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX pass ' "$summary" |
    sed -E 's/.* overlay=([^ ]+) log=.*/\1/' | sort -u | wc -l)
if (( pass_count != rounds * 2 || unique_overlays != pass_count )); then
    echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX overlay-isolation-failed passes=$pass_count unique_overlays=$unique_overlays" >&2
    exit 1
fi

elapsed_s=$(( $(date +%s) - started_at ))
echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX PASS rounds=$rounds total=$((rounds * 2)) elapsed_s=$elapsed_s"
echo "BUILDSTORM_STAGE8_NESTED_QEMU_MATRIX summary=$summary"
