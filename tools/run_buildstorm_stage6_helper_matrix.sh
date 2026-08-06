#!/usr/bin/env bash
#
# Validate the official-aligned, untimed precompiled tg-xtask helper step.
# Architecture lanes run concurrently; each lane uses consecutive cold boots
# with an immutable raw base and a fresh qcow2 overlay per round.

set -euo pipefail

rounds=${1:-2}
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
official_tests_commit=$(git -C contest/testsuits-for-oskernel rev-parse --verify HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
summary=${BUILDSTORM_MATRIX_SUMMARY:-"$state_dir/probes/stage6-helper-matrix-${commit}-${timestamp}.txt"}
mkdir -p "$state_dir/probes"

if [[ ${BUILDSTORM_MATRIX_INTERNAL:-0} != 1 ]]; then
    export BUILDSTORM_MATRIX_INTERNAL=1
    export BUILDSTORM_MATRIX_SUMMARY="$summary"
    set +e
    timeout --signal=TERM --kill-after=30 "$budget_s" bash "$0" "$rounds"
    status=$?
    set -e
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "BUILDSTORM_STAGE6_HELPER_MATRIX TIME_BUDGET_EXCEEDED budget_s=$budget_s" |
            tee -a "$summary" >&2
    fi
    exit "$status"
fi

exec > >(tee "$summary") 2>&1
matrix_work_dir=$(mktemp -d /tmp/a20os-buildstorm-stage6-helper.XXXXXX)
export FINAL_EVAL_IMAGE_CACHE_DIR=${FINAL_EVAL_IMAGE_CACHE_DIR:-/tmp/a20os-buildstorm-matrix-cache}
export FINAL_EVAL_WORK_DIR="$matrix_work_dir"
cleanup() {
    rm -rf -- "$matrix_work_dir"
}
trap cleanup EXIT

echo "BUILDSTORM_STAGE6_HELPER_MATRIX commit=$commit rounds=$rounds parallelism=$parallelism budget_s=$budget_s"
echo "BUILDSTORM_STAGE6_HELPER_MATRIX official_tests_commit=$official_tests_commit"
echo "BUILDSTORM_STAGE6_HELPER_MATRIX cache=$FINAL_EVAL_IMAGE_CACHE_DIR work=$FINAL_EVAL_WORK_DIR"
started_at=$(date +%s)

exact_lifetime_fields='^(task_objects|task_refs|listed_tasks|listed_refs|pid_entries|runqueue_entries|dispatching_tasks|cpu_owned_tasks|wait_entries|wake_entries|timeout_entries|open_fds|vfile_objects|page_cache_dirty|page_cache_pinned|zombies|lifetime_errors):'

metadata_value() {
    local key=$1
    local metadata=$2
    sed -n "s/^${key}=//p" "$metadata" | tail -n 1
}

run_lane() {
    local arch=$1
    local target expected_hash expected_helper_size expected_target
    local round log metadata before after lifetime_lines
    local vnode_before vnode_after overlay helper_before helper_after elapsed helper_host_log

    case "$arch" in
    riscv64)
        target=final-stage6-rv-helper
        expected_hash=cba87f43ae569bcf2b8e4614f75cec1bf51bedb2804626fe466fcce3861df6f1
        expected_helper_size=277827240
        expected_target=/work/tgoskits/target/riscv64gc-unknown-linux-musl
        ;;
    loongarch64)
        target=final-stage6-la-helper
        expected_hash=2c411447274fbd83505d2fac505a5d9e8ed8ff3bdfc3d2d6cbdb8f61ff7d90d2
        expected_helper_size=280919472
        expected_target=/work/tgoskits/target/loongarch64-unknown-linux-musl
        ;;
    esac

    for ((round = 1; round <= rounds; round++)); do
        echo "BUILDSTORM_STAGE6_HELPER_MATRIX start arch=$arch round=$round/$rounds"
        make "$target"
        log=$(ls -1t \
            "$state_dir"/logs/"${arch}-buildstorm-probe-8c-stage6-precompiled-helper-"*.log |
            head -n 1)
        metadata=${log/\/logs\//\/metadata\/}
        metadata=${metadata%.log}.txt

        rg -q '^BUILDSTORM_STAGE6_PRECOMPILED_HELPER ok$' "$log"
        rg -q '^\[BUILDSTORM-PROBE\] summary total=1 failures=0$' "$log"
        if rg -q 'SIGSEGV:|code=13|code=3|Ecode=0x1[01]|lifetime_errors: [1-9]|Kernel panic|Out of memory|unhandled exception|^E: /a20-buildstorm-probe' "$log"; then
            echo "BUILDSTORM_STAGE6_HELPER_MATRIX unexpected-fault log=$log" >&2
            return 1
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
            echo "BUILDSTORM_STAGE6_HELPER_MATRIX resource-baseline-mismatch log=$log" >&2
            diff -u "$before" "$after" >&2 || true
            rm -f "$before" "$after"
            return 1
        fi
        rm -f "$before" "$after"
        vnode_before=$(awk '/^vnode_objects:/{print $2; exit}' "$log")
        vnode_after=$(awk '/^vnode_objects:/{value=$2} END{print value}' "$log")
        if (( vnode_after > vnode_before )); then
            echo "BUILDSTORM_STAGE6_HELPER_MATRIX vnode-growth before=$vnode_before after=$vnode_after log=$log" >&2
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

        [[ $(metadata_value tg_xtask_timing_scope "$metadata") == untimed_helper_check ]]
        [[ $(metadata_value tg_xtask_before_path "$metadata") == /work/tgoskits/target/debug/tg-xtask ]]
        [[ $(metadata_value tg_xtask_before_mode "$metadata") == 755 ]]
        [[ $(metadata_value tg_xtask_before_executable "$metadata") == yes ]]
        [[ $(metadata_value tg_xtask_cache_fingerprint "$metadata") == present ]]
        [[ $(metadata_value tg_xtask_cache_deps "$metadata") == present ]]
        [[ $(metadata_value tg_xtask_cache_build "$metadata") == present ]]
        [[ $(metadata_value tg_xtask_target_cleanup "$metadata") == "$expected_target" ]]
        [[ $(metadata_value tg_xtask_build_output "$metadata") == /work/a20-stage6-precompiled-helper.out ]]
        [[ $(metadata_value tg_xtask_build_rc "$metadata") == 0 ]]
        [[ $(metadata_value tg_xtask_after_path "$metadata") == /work/tgoskits/target/debug/tg-xtask ]]
        [[ $(metadata_value tg_xtask_after_mode "$metadata") == 755 ]]
        [[ $(metadata_value tg_xtask_after_executable "$metadata") == yes ]]

        helper_before=$(metadata_value tg_xtask_before_bytes "$metadata")
        helper_after=$(metadata_value tg_xtask_after_bytes "$metadata")
        [[ "$helper_before" =~ ^[1-9][0-9]*$ && "$helper_after" =~ ^[1-9][0-9]*$ ]]
        (( helper_before == expected_helper_size ))
        (( helper_after > 0 ))
        elapsed=$(metadata_value tg_xtask_build_elapsed_s "$metadata")
        [[ "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ ]]
        helper_host_log=$(metadata_value tg_xtask_build_host_log "$metadata")
        [[ -s "$helper_host_log" ]]
        rg -q 'Finished `dev` profile' "$helper_host_log"

        overlay=$(metadata_value official_image_overlay "$metadata")
        [[ -n "$overlay" ]]
        echo "BUILDSTORM_STAGE6_HELPER_MATRIX pass arch=$arch round=$round/$rounds elapsed_s=$elapsed overlay=$overlay log=$log"
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
    echo "BUILDSTORM_STAGE6_HELPER_MATRIX FAIL failed_lanes=$failures" >&2
    exit 1
fi

pass_count=$(rg -c '^BUILDSTORM_STAGE6_HELPER_MATRIX pass ' "$summary")
unique_overlays=$(rg '^BUILDSTORM_STAGE6_HELPER_MATRIX pass ' "$summary" |
    sed -E 's/.* overlay=([^ ]+) log=.*/\1/' | sort -u | wc -l)
if (( pass_count != rounds * 2 || unique_overlays != pass_count )); then
    echo "BUILDSTORM_STAGE6_HELPER_MATRIX overlay-isolation-failed passes=$pass_count unique_overlays=$unique_overlays" >&2
    exit 1
fi

elapsed_s=$(( $(date +%s) - started_at ))
echo "BUILDSTORM_STAGE6_HELPER_MATRIX PASS rounds=$rounds total=$((rounds * 2)) elapsed_s=$elapsed_s"
echo "BUILDSTORM_STAGE6_HELPER_MATRIX summary=$summary"
