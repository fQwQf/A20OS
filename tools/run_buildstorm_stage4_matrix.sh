#!/usr/bin/env bash
#
# Run the one-hour BuildStorm stage-4 regression matrix.  Each architecture
# and CPU-count lane is sequential, while independent lanes run concurrently.
# Every boot still gets an isolated overlay, run directory, log, and metadata.

set -euo pipefail

rounds=${1:-3}
state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}
parallelism=${BUILDSTORM_MATRIX_PARALLELISM:-2}
budget_s=${BUILDSTORM_MATRIX_BUDGET_SECONDS:-3600}

if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-round-count]" >&2
    exit 2
fi
if [[ "$parallelism" != 2 && "$parallelism" != 3 ]]; then
    echo "BUILDSTORM_MATRIX_PARALLELISM must be 2 or 3" >&2
    exit 2
fi
if [[ ! "$budget_s" =~ ^[1-9][0-9]*$ ]]; then
    echo "BUILDSTORM_MATRIX_BUDGET_SECONDS must be positive" >&2
    exit 2
fi

commit=$(git rev-parse --verify HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
summary=${BUILDSTORM_MATRIX_SUMMARY:-"$state_dir/probes/stage4-matrix-${commit}-${timestamp}.txt"}
mkdir -p "$state_dir/probes"

if [[ ${BUILDSTORM_MATRIX_INTERNAL:-0} != 1 ]]; then
    export BUILDSTORM_MATRIX_INTERNAL=1
    export BUILDSTORM_MATRIX_SUMMARY="$summary"
    set +e
    timeout --signal=TERM --kill-after=30 "$budget_s" bash "$0" "$rounds"
    status=$?
    set -e
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "BUILDSTORM_STAGE4_MATRIX TIME_BUDGET_EXCEEDED budget_s=$budget_s" |
            tee -a "$summary" >&2
    fi
    exit "$status"
fi

exec > >(tee "$summary") 2>&1
matrix_work_dir=$(mktemp -d /tmp/a20os-buildstorm-stage4.XXXXXX)
export FINAL_EVAL_IMAGE_CACHE_DIR=${FINAL_EVAL_IMAGE_CACHE_DIR:-/tmp/a20os-buildstorm-matrix-cache}
export FINAL_EVAL_WORK_DIR="$matrix_work_dir"
cleanup() {
    rm -rf -- "$matrix_work_dir"
}
trap cleanup EXIT

echo "BUILDSTORM_STAGE4_MATRIX commit=$commit rounds=$rounds parallelism=$parallelism budget_s=$budget_s"
echo "BUILDSTORM_STAGE4_MATRIX cache=$FINAL_EVAL_IMAGE_CACHE_DIR work=$FINAL_EVAL_WORK_DIR"
started_at=$(date +%s)

run_lane() {
    local arch=$1
    local target_arch=$2
    local cores=$3
    local target="final-stage4-${target_arch}-buildstorm-${cores}c"
    local round log metadata
    for ((round = 1; round <= rounds; round++)); do
        echo "BUILDSTORM_STAGE4_MATRIX start arch=$arch cores=$cores round=$round/$rounds"
        make "$target"
        log=$(ls -1t \
            "$state_dir"/logs/"${arch}-buildstorm-probe-${cores}c-stage4-cargo-minibuild-"*.log |
            sed -n '1p')
        metadata=${log/\/logs\//\/metadata\/}
        metadata=${metadata%.log}.txt

        rg -q '^BUILDSTORM_STAGE4_CARGO_MINIBUILD ok$' "$log"
        rg -q '^\[BUILDSTORM-PROBE\] summary total=1 failures=0$' "$log"
        [[ $(rg -c '^Hello, world!$' "$log") -ge 2 ]]
        rg -q '^lifetime_errors: 0$' "$log"
        if [[ "$arch" == loongarch64 ]]; then
            rg -q '^ARCH_CONTEXT_STRESS: signal-lsx-fcc PASS$' "$log"
            rg -q '^ARCH_CONTEXT_STRESS: schedule-lsx-fcc PASS$' "$log"
        fi
        if rg -q 'SIGSEGV:|code=13|code=3|Ecode=0x1[01]|lifetime_errors: [1-9]' "$log"; then
            echo "BUILDSTORM_STAGE4_MATRIX unexpected-fault log=$log" >&2
            return 1
        fi
        rg -q '^qemu_exit_status=0$' "$metadata"
        rg -q '^qemu_timed_out=no$' "$metadata"
        rg -q "^guest_cores=${cores}/${cores}$" "$metadata"
        echo "BUILDSTORM_STAGE4_MATRIX pass arch=$arch cores=$cores round=$round/$rounds log=$log"
    done
}

pids=()
labels=()
failures=0
for lane in "riscv64 rv 1" "riscv64 rv 8" \
            "loongarch64 la 1" "loongarch64 la 8"; do
    read -r arch target_arch cores <<<"$lane"
    run_lane "$arch" "$target_arch" "$cores" &
    pids+=("$!")
    labels+=("${arch}-${cores}c")
    if (( ${#pids[@]} >= parallelism )); then
        status=0
        wait "${pids[0]}" || status=$?
        echo "BUILDSTORM_STAGE4_MATRIX lane-status label=${labels[0]} pid=${pids[0]} status=$status"
        if (( status != 0 )); then
            failures=$((failures + 1))
        fi
        pids=("${pids[@]:1}")
        labels=("${labels[@]:1}")
    fi
done
for index in "${!pids[@]}"; do
    status=0
    wait "${pids[index]}" || status=$?
    echo "BUILDSTORM_STAGE4_MATRIX lane-status label=${labels[index]} pid=${pids[index]} status=$status"
    if (( status != 0 )); then
        failures=$((failures + 1))
    fi
done
if (( failures != 0 )); then
    echo "BUILDSTORM_STAGE4_MATRIX FAIL failed_lanes=$failures" >&2
    exit 1
fi

elapsed_s=$(( $(date +%s) - started_at ))
echo "BUILDSTORM_STAGE4_MATRIX PASS rounds=$rounds total=$((rounds * 4)) elapsed_s=$elapsed_s"
echo "BUILDSTORM_STAGE4_MATRIX summary=$summary"
