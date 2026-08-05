#!/usr/bin/env bash
#
# Run the BuildStorm stage-6 tg-xtask jobs=1 correctness matrix.
# Architecture lanes execute concurrently, every run gets an independent
# qcow2 overlay, and the one-hour target is report-only.  The per-QEMU safety
# watchdog is deliberately separate and never shortened by the soft target.

set -euo pipefail

rounds=${1:-1}
state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}
parallelism=${BUILDSTORM_MATRIX_PARALLELISM:-2}
soft_target_s=${BUILDSTORM_STAGE6_SOFT_TARGET_SECONDS:-3600}
qemu_watchdog_s=${BUILDSTORM_STAGE6_QEMU_WATCHDOG_SECONDS:-28800}
host_make_jobs=${BUILDSTORM_HOST_MAKE_JOBS:-20}

if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-round-count]" >&2
    exit 2
fi
if [[ "$parallelism" != 2 && "$parallelism" != 3 ]]; then
    echo "BUILDSTORM_MATRIX_PARALLELISM must be 2 or 3" >&2
    exit 2
fi
for value_name in soft_target_s qemu_watchdog_s host_make_jobs; do
    value=${!value_name}
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "$value_name must be a positive integer" >&2
        exit 2
    fi
done
if (( qemu_watchdog_s <= 27000 )); then
    echo "BUILDSTORM_STAGE6_QEMU_WATCHDOG_SECONDS must exceed the 27000-second guest case watchdog" >&2
    exit 2
fi

commit=$(git rev-parse --verify HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
summary=${BUILDSTORM_STAGE6_MATRIX_SUMMARY:-"$state_dir/probes/stage6-matrix-${commit}-${timestamp}.txt"}
matrix_work_dir=${FINAL_EVAL_WORK_DIR:-"$state_dir/runs/stage6-matrix-${commit}-${timestamp}"}
mkdir -p "$state_dir/probes" "$matrix_work_dir"

exec > >(tee "$summary") 2>&1
export FINAL_EVAL_IMAGE_CACHE_DIR=${FINAL_EVAL_IMAGE_CACHE_DIR:-/tmp/a20os-buildstorm-matrix-cache}
export FINAL_EVAL_WORK_DIR="$matrix_work_dir"

echo "BUILDSTORM_STAGE6_MATRIX commit=$commit rounds=$rounds parallelism=$parallelism"
echo "BUILDSTORM_STAGE6_MATRIX soft_target_s=$soft_target_s qemu_watchdog_s=$qemu_watchdog_s host_make_jobs=$host_make_jobs"
echo "BUILDSTORM_STAGE6_MATRIX cache=$FINAL_EVAL_IMAGE_CACHE_DIR work=$FINAL_EVAL_WORK_DIR"
started_at=$(date +%s)
runner_pid=$$
soft_reporter_pid=

(
    sleep "$soft_target_s"
    if kill -0 "$runner_pid" 2>/dev/null; then
        elapsed_s=$(( $(date +%s) - started_at ))
        echo "BUILDSTORM_STAGE6_MATRIX SOFT_TARGET_EXCEEDED target_s=$soft_target_s elapsed_s=$elapsed_s action=continue"
    fi
) &
soft_reporter_pid=$!

stop_soft_reporter() {
    if [[ -n "$soft_reporter_pid" ]]; then
        kill "$soft_reporter_pid" 2>/dev/null || true
        wait "$soft_reporter_pid" 2>/dev/null || true
    fi
}
trap stop_soft_reporter EXIT

exact_lifetime_fields='^(task_objects|task_refs|listed_tasks|listed_refs|pid_entries|runqueue_entries|dispatching_tasks|cpu_owned_tasks|wait_entries|wake_entries|timeout_entries|open_fds|vfile_objects|page_cache_dirty|page_cache_pinned|zombies|lifetime_errors):'
zero_error_fields='^(timeout_full_failures|timeout_duplicate_rejections|timeout_heap_violations|scheduler_violations|ref_get_failures|ref_underflows|duplicate_destroy|bad_final_put|state_violations|lifetime_errors): [1-9]'

run_lane() {
    local arch=$1
    local target round invocation status log metadata base_image mode
    local before after lifetime_lines artifact_bytes
    local vnode_before vnode_after page_before page_after frames_before frames_after
    case "$arch" in
    riscv64) target=final-stage6-rv-buildstorm-j1 ;;
    loongarch64) target=final-stage6-la-buildstorm-j1 ;;
    *) return 2 ;;
    esac

    for ((round = 1; round <= rounds; round++)); do
        echo "BUILDSTORM_STAGE6_MATRIX start arch=$arch jobs=1 round=$round/$rounds"
        invocation="$matrix_work_dir/${arch}-j1-round-${round}.host.log"
        set +e
        make -j"$host_make_jobs" \
            BUILDSTORM_STAGE6_QEMU_TIMEOUT_SECONDS="$qemu_watchdog_s" \
            "$target" 2>&1 | tee "$invocation"
        status=${PIPESTATUS[0]}
        set -e
        if (( status != 0 )); then
            echo "BUILDSTORM_STAGE6_MATRIX invocation-failed arch=$arch jobs=1 round=$round status=$status host_log=$invocation" >&2
            return "$status"
        fi

        log=$(sed -nE 's#^\[final-eval\] serial:[[:space:]]+##p' "$invocation" | tail -n 1)
        metadata=$(sed -nE 's#^\[final-eval\] metadata:[[:space:]]+##p' "$invocation" | tail -n 1)
        if [[ -z "$log" || ! -r "$log" || -z "$metadata" || ! -r "$metadata" ]]; then
            echo "BUILDSTORM_STAGE6_MATRIX missing-artifacts arch=$arch host_log=$invocation" >&2
            return 1
        fi

        rg -q '^BUILDSTORM_STAGE6_TG_XTASK command=cargo build -p tg-xtask --jobs 1$' "$log"
        rg -q "^BUILDSTORM_STAGE6_TG_XTASK result arch=$arch jobs=1 rc=0 output=/work/a20-stage6-tg-xtask-j1.stdout-stderr.log$" "$log"
        rg -q "^BUILDSTORM_STAGE6_TG_XTASK ok arch=$arch jobs=1$" "$log"
        rg -q '^\[BUILDSTORM-PROBE\] case=stage6-tg-xtask-j1 rc=0$' "$log"
        rg -q '^\[BUILDSTORM-PROBE\] summary total=1 failures=0$' "$log"

        artifact_bytes=$(sed -nE 's#^BUILDSTORM_STAGE6_TG_XTASK artifact=/work/tgoskits/target/debug/tg-xtask bytes=([0-9]+) executable=yes$#\1#p' "$log" | tail -n 1)
        if [[ -z "$artifact_bytes" ]] || (( artifact_bytes <= 0 )); then
            echo "BUILDSTORM_STAGE6_MATRIX invalid-artifact log=$log" >&2
            return 1
        fi
        if rg -q 'SIGSEGV:|Kernel panic|kernel panic|panic:|panicked at|fatal runtime error|Out of memory|OOM killed|\[A20\] UNHANDLED syscall:|\[SYSCALL\] Unimplemented:|code=13|Ecode=0x1[01]' "$log"; then
            echo "BUILDSTORM_STAGE6_MATRIX unexpected-fault log=$log" >&2
            return 1
        fi
        if rg -q "$zero_error_fields" "$log"; then
            echo "BUILDSTORM_STAGE6_MATRIX lifetime-error log=$log" >&2
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
            echo "BUILDSTORM_STAGE6_MATRIX resource-baseline-mismatch log=$log" >&2
            diff -u "$before" "$after" >&2 || true
            rm -f "$before" "$after"
            return 1
        fi
        rm -f "$before" "$after"

        vnode_before=$(awk '/^vnode_objects:/{print $2; exit}' "$log")
        vnode_after=$(awk '/^vnode_objects:/{value=$2} END{print value}' "$log")
        page_before=$(awk '/^page_cache_valid:/{print $2; exit}' "$log")
        page_after=$(awk '/^page_cache_valid:/{value=$2} END{print value}' "$log")
        frames_before=$(awk '/^free_frames:/{print $2; exit}' "$log")
        frames_after=$(awk '/^free_frames:/{value=$2} END{print value}' "$log")
        if [[ -z "$vnode_before" || -z "$vnode_after" || -z "$page_before" || -z "$page_after" || -z "$frames_before" || -z "$frames_after" ]]; then
            echo "BUILDSTORM_STAGE6_MATRIX incomplete-lifetime-snapshot log=$log" >&2
            return 1
        fi
        if (( vnode_after > vnode_before || page_after > page_before )); then
            echo "BUILDSTORM_STAGE6_MATRIX cache-growth vnode=$vnode_before:$vnode_after page=$page_before:$page_after log=$log" >&2
            return 1
        fi

        rg -q "^git_commit=$commit$" "$metadata"
        rg -q '^git_dirty=no$' "$metadata"
        rg -q '^group=buildstorm-probe$' "$metadata"
        rg -q '^probe_case=stage6-tg-xtask-j1$' "$metadata"
        rg -q '^qemu_exit_status=0$' "$metadata"
        rg -q '^qemu_timed_out=no$' "$metadata"
        rg -q '^guest_cores=8/8$' "$metadata"
        rg -q "^qemu_timeout_s=$qemu_watchdog_s$" "$metadata"
        base_image=$(sed -n 's/^official_image_base=//p' "$metadata")
        mode=$(stat -c '%a' "$base_image")
        if [[ ! -r "$base_image" || "$mode" != 444 ]]; then
            echo "BUILDSTORM_STAGE6_MATRIX mutable-or-missing-base path=$base_image mode=$mode" >&2
            return 1
        fi

        echo "BUILDSTORM_STAGE6_MATRIX resources arch=$arch jobs=1 round=$round vnode=$vnode_before:$vnode_after page=$page_before:$page_after free_frames=$frames_before:$frames_after"
        echo "BUILDSTORM_STAGE6_MATRIX pass arch=$arch jobs=1 round=$round/$rounds bytes=$artifact_bytes log=$log metadata=$metadata"
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
    echo "BUILDSTORM_STAGE6_MATRIX FAIL failed_lanes=$failures" >&2
    exit 1
fi

elapsed_s=$(( $(date +%s) - started_at ))
if (( elapsed_s > soft_target_s )); then
    echo "BUILDSTORM_STAGE6_MATRIX soft_target_met=no target_s=$soft_target_s elapsed_s=$elapsed_s"
else
    echo "BUILDSTORM_STAGE6_MATRIX soft_target_met=yes target_s=$soft_target_s elapsed_s=$elapsed_s"
fi
echo "BUILDSTORM_STAGE6_MATRIX PASS rounds=$rounds total=$((rounds * 2)) elapsed_s=$elapsed_s"
echo "BUILDSTORM_STAGE6_MATRIX summary=$summary"
