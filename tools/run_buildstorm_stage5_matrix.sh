#!/usr/bin/env bash
#
# Run the one-hour BuildStorm stage-5 official toolchain/minibuild matrix.
# Architecture lanes execute concurrently; rounds within a lane stay
# sequential so every result can be associated with its exact archived log.

set -euo pipefail

rounds=${1:-5}
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
summary=${BUILDSTORM_MATRIX_SUMMARY:-"$state_dir/probes/stage5-matrix-${commit}-${timestamp}.txt"}
mkdir -p "$state_dir/probes"

if [[ ${BUILDSTORM_MATRIX_INTERNAL:-0} != 1 ]]; then
    export BUILDSTORM_MATRIX_INTERNAL=1
    export BUILDSTORM_MATRIX_SUMMARY="$summary"
    set +e
    timeout --signal=TERM --kill-after=30 "$budget_s" bash "$0" "$rounds"
    status=$?
    set -e
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "BUILDSTORM_STAGE5_MATRIX TIME_BUDGET_EXCEEDED budget_s=$budget_s" |
            tee -a "$summary" >&2
    fi
    exit "$status"
fi

exec > >(tee "$summary") 2>&1
matrix_work_dir=$(mktemp -d /tmp/a20os-buildstorm-stage5.XXXXXX)
export FINAL_EVAL_IMAGE_CACHE_DIR=${FINAL_EVAL_IMAGE_CACHE_DIR:-/tmp/a20os-buildstorm-matrix-cache}
export FINAL_EVAL_WORK_DIR="$matrix_work_dir"
cleanup() {
    rm -rf -- "$matrix_work_dir"
}
trap cleanup EXIT

echo "BUILDSTORM_STAGE5_MATRIX commit=$commit rounds=$rounds parallelism=$parallelism budget_s=$budget_s"
echo "BUILDSTORM_STAGE5_MATRIX cache=$FINAL_EVAL_IMAGE_CACHE_DIR work=$FINAL_EVAL_WORK_DIR"
started_at=$(date +%s)

exact_lifetime_fields='^(task_objects|task_refs|listed_tasks|listed_refs|pid_entries|runqueue_entries|dispatching_tasks|cpu_owned_tasks|wait_entries|wake_entries|timeout_entries|open_fds|vfile_objects|page_cache_dirty|page_cache_pinned|zombies|lifetime_errors):'

run_lane() {
    local arch=$1
    local target round log metadata before after lifetime_lines
    local vnode_before vnode_after
    case "$arch" in
    riscv64) target=final-stage5-rv-buildstorm ;;
    loongarch64) target=final-stage5-la-buildstorm ;;
    esac
    for ((round = 1; round <= rounds; round++)); do
        echo "BUILDSTORM_STAGE5_MATRIX start arch=$arch round=$round/$rounds"
        make "$target"
        log=$(ls -1t \
            "$state_dir"/logs/"${arch}-buildstorm-probe-8c-stage5-official-minibuild-"*.log |
            sed -n '1p')
        metadata=${log/\/logs\//\/metadata\/}
        metadata=${metadata%.log}.txt

        rg -q '^BUILDSTORM_TOOLCHAIN ok$' "$log"
        rg -q '^BUILDSTORM_MINIBUILD ok$' "$log"
        rg -q '^Hello, world!$' "$log"
        rg -q '^\[BUILDSTORM-PROBE\] summary total=1 failures=0$' "$log"
        if rg -q 'SIGSEGV:|code=13|code=3|Ecode=0x1[01]|lifetime_errors: [1-9]' "$log"; then
            echo "BUILDSTORM_STAGE5_MATRIX unexpected-fault log=$log" >&2
            exit 1
        fi

        before=$(mktemp)
        after=$(mktemp)
        lifetime_lines=$(mktemp)
        rg "$exact_lifetime_fields" "$log" >"$lifetime_lines"
        awk -F': ' '
            !seen[$1]++ {
                order[++count] = $1
                printf "%s: %lu\n", $1, $2 + 0
            }
        ' "$lifetime_lines" >"$before"
        awk -F': ' '
            !seen[$1]++ { order[++count] = $1 }
            { value[$1] = $2 + 0 }
            END {
                for (i = 1; i <= count; i++)
                    printf "%s: %lu\n", order[i], value[order[i]]
            }
        ' "$lifetime_lines" >"$after"
        rm -f "$lifetime_lines"
        if ! cmp -s "$before" "$after"; then
            echo "BUILDSTORM_STAGE5_MATRIX resource-baseline-mismatch log=$log" >&2
            diff -u "$before" "$after" >&2 || true
            rm -f "$before" "$after"
            exit 1
        fi
        vnode_before=$(awk '/^vnode_objects:/{print $2; exit}' "$log")
        vnode_after=$(awk '/^vnode_objects:/{value=$2} END{print value}' "$log")
        if (( vnode_after > vnode_before )); then
            echo "BUILDSTORM_STAGE5_MATRIX vnode-growth before=$vnode_before after=$vnode_after log=$log" >&2
            exit 1
        fi
        rm -f "$before" "$after"

        rg -q '^qemu_exit_status=0$' "$metadata"
        rg -q '^qemu_timed_out=no$' "$metadata"
        rg -q '^guest_cores=8/8$' "$metadata"
        echo "BUILDSTORM_STAGE5_MATRIX pass arch=$arch round=$round/$rounds log=$log"
    done
}

pids=()
labels=()
for arch in riscv64 loongarch64; do
    run_lane "$arch" &
    pids+=("$!")
    labels+=("$arch")
done
failures=0
for index in "${!pids[@]}"; do
    status=0
    wait "${pids[index]}" || status=$?
    echo "BUILDSTORM_STAGE5_MATRIX lane-status label=${labels[index]} pid=${pids[index]} status=$status"
    if (( status != 0 )); then
        failures=$((failures + 1))
    fi
done
if (( failures != 0 )); then
    echo "BUILDSTORM_STAGE5_MATRIX FAIL failed_lanes=$failures" >&2
    exit 1
fi

elapsed_s=$(( $(date +%s) - started_at ))
echo "BUILDSTORM_STAGE5_MATRIX PASS rounds=$rounds total=$((rounds * 2)) elapsed_s=$elapsed_s"
echo "BUILDSTORM_STAGE5_MATRIX summary=$summary"
