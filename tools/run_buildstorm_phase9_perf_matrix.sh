#!/usr/bin/env bash
# Run the combined Phase-9 feedback probe serially on both CPU counts and arches.

set -euo pipefail

rounds=${1:-1}
state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}
budget_s=${PHASE9_MATRIX_BUDGET_SECONDS:-7200}
performance_lock="$state_dir/locks/stage9-performance-qemu.lock"

if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-round-count]" >&2
    exit 2
fi
if [[ ! "$budget_s" =~ ^[1-9][0-9]*$ ]]; then
    echo "PHASE9_MATRIX_BUDGET_SECONDS must be positive" >&2
    exit 2
fi

commit=$(git rev-parse --verify HEAD)
official_tests_commit=$(git -C contest/testsuits-for-oskernel rev-parse --verify HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
summary=${PHASE9_MATRIX_SUMMARY:-"$state_dir/probes/phase9-perf-matrix-${commit}-${timestamp}.txt"}
mkdir -p "$state_dir/probes" "$state_dir/locks"

if [[ ${PHASE9_MATRIX_INTERNAL:-0} != 1 ]]; then
    export PHASE9_MATRIX_INTERNAL=1
    export PHASE9_MATRIX_SUMMARY="$summary"
    set +e
    timeout --signal=TERM --kill-after=30 "$budget_s" bash "$0" "$rounds"
    status=$?
    set -e
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "BUILDSTORM_PHASE9_PERF_MATRIX TIME_BUDGET_EXCEEDED budget_s=$budget_s" |
            tee -a "$summary" >&2
    fi
    exit "$status"
fi

exec > >(tee "$summary") 2>&1
echo "BUILDSTORM_PHASE9_PERF_MATRIX child_performance_lock=$performance_lock"

matrix_work_dir=$(mktemp -d /tmp/a20os-buildstorm-phase9.XXXXXX)
export FINAL_EVAL_IMAGE_CACHE_DIR=${FINAL_EVAL_IMAGE_CACHE_DIR:-/tmp/a20os-buildstorm-matrix-cache}
export FINAL_EVAL_WORK_DIR="$matrix_work_dir"
cleanup() {
    rm -rf -- "$matrix_work_dir"
}
trap cleanup EXIT

echo "BUILDSTORM_PHASE9_PERF_MATRIX commit=$commit rounds=$rounds budget_s=$budget_s"
echo "BUILDSTORM_PHASE9_PERF_MATRIX official_tests_commit=$official_tests_commit"
echo "BUILDSTORM_PHASE9_PERF_MATRIX cache=$FINAL_EVAL_IMAGE_CACHE_DIR work=$FINAL_EVAL_WORK_DIR"
started_at=$(date +%s)

counters=(
    page_cache_scan_calls page_cache_scan_entries
    vfs_time_meta_calls vfs_time_meta_probes
    ext4_group_probes ext4_bitmap_probes
    mm_tlb_transactions mm_tlb_transaction_flushes mm_tlb_remote_cpus
    virtio_blk_polls virtio_blk_active_polls virtio_blk_used_checks
    virtio_blk_completions idle_wait_attempts idle_wait_entries
    idle_wait_wake_returns
)
subloads=(
    idle stat-open-close write-truncate-4k ext4-create-allocation
    overwrite mmap-munmap sync-cleanup
)
lifetime_field_names='task_objects task_refs listed_tasks listed_refs pid_entries runqueue_entries dispatching_tasks cpu_owned_tasks wait_entries wake_entries timeout_entries open_fds vfile_objects vnode_objects page_cache_dirty page_cache_pinned zombies lifetime_errors'
lifetime_fields='^(task_objects|task_refs|listed_tasks|listed_refs|pid_entries|runqueue_entries|dispatching_tasks|cpu_owned_tasks|wait_entries|wake_entries|timeout_entries|open_fds|vfile_objects|vnode_objects|page_cache_dirty|page_cache_pinned|zombies|lifetime_errors):'
zero_after_fields='page_cache_dirty page_cache_pinned zombies lifetime_errors'

metadata_value() {
    local key=$1
    local metadata=$2
    sed -n "s/^${key}=//p" "$metadata" | tail -n 1
}

audit_snapshots() {
    local log=$1
    local counter count label
    local actual expected
    actual=$(mktemp)
    expected=$(mktemp)

    awk '/^STAGE9_PERF_(SNAPSHOT|SUBLOAD)/ { print }' "$log" >"$actual"
    {
        for label in initial "${subloads[@]}"; do
            if [[ "$label" != initial ]]; then
                printf 'STAGE9_PERF_SUBLOAD start name=%s\n' "$label"
                printf 'STAGE9_PERF_SUBLOAD end name=%s rc=0\n' "$label"
            fi
            printf 'STAGE9_PERF_SNAPSHOT label=%s source=perf begin\n' "$label"
            printf 'STAGE9_PERF_SNAPSHOT label=%s source=perf end\n' "$label"
            printf 'STAGE9_PERF_SNAPSHOT label=%s source=task_lifetime begin\n' "$label"
            printf 'STAGE9_PERF_SNAPSHOT label=%s source=task_lifetime end\n' "$label"
        done
    } >"$expected"
    if ! cmp -s "$expected" "$actual"; then
        echo "BUILDSTORM_PHASE9_PERF_MATRIX marker-order-invalid log=$log" >&2
        diff -u "$expected" "$actual" >&2 || true
        rm -f "$actual" "$expected"
        return 1
    fi
    rm -f "$actual" "$expected"

    for counter in "${counters[@]}"; do
        count=$(rg -c "^${counter}: [0-9]+$" "$log")
        if (( count != 8 )); then
            echo "BUILDSTORM_PHASE9_PERF_MATRIX counter-snapshot-count counter=$counter count=$count log=$log" >&2
            return 1
        fi
        if ! awk -F': ' -v key="$counter" '
            $1 == key {
                if (seen && $2 + 0 < previous)
                    exit 1
                previous = $2 + 0
                seen++
            }
            END { if (seen != 8) exit 1 }
        ' "$log"; then
            echo "BUILDSTORM_PHASE9_PERF_MATRIX counter-not-monotonic counter=$counter log=$log" >&2
            return 1
        fi
    done
}

audit_outer_lifetime() {
    local log=$1
    local lifetime_lines
    lifetime_lines=$(mktemp)
    awk -v fields="$lifetime_fields" '
        $0 == "[BUILDSTORM-PROBE][LIFETIME][stage9-perf-feedback:before] begin" {
            capture = 1
            next
        }
        $0 == "[BUILDSTORM-PROBE][LIFETIME][stage9-perf-feedback:before] end" {
            capture = 0
            next
        }
        $0 == "[BUILDSTORM-PROBE][LIFETIME][stage9-perf-feedback:after] begin" {
            capture = 1
            next
        }
        $0 == "[BUILDSTORM-PROBE][LIFETIME][stage9-perf-feedback:after] end" {
            capture = 0
            next
        }
        capture && $0 ~ fields { print }
    ' "$log" >"$lifetime_lines"
    if ! awk -F': ' \
        -v required="$lifetime_field_names" \
        -v zero_after="$zero_after_fields" '
        BEGIN {
            required_count = split(required, required_fields, " ")
            zero_count = split(zero_after, zero_fields, " ")
            for (i = 1; i <= zero_count; i++)
                must_be_zero[zero_fields[i]] = 1
        }
        {
            count[$1]++
            if (count[$1] == 1)
                before[$1] = $2 + 0
            after[$1] = $2 + 0
        }
        END {
            failed = 0
            for (i = 1; i <= required_count; i++) {
                field = required_fields[i]
                if (count[field] != 2) {
                    printf "outer lifetime field=%s count=%u\n", field,
                        count[field] > "/dev/stderr"
                    failed = 1
                    continue
                }
                if (after[field] > before[field]) {
                    printf "outer lifetime growth field=%s before=%u after=%u\n",
                        field, before[field], after[field] > "/dev/stderr"
                    failed = 1
                }
                if (must_be_zero[field] && after[field] != 0) {
                    printf "outer lifetime terminal field=%s after=%u\n",
                        field, after[field] > "/dev/stderr"
                    failed = 1
                }
            }
            exit failed
        }
    ' "$lifetime_lines"; then
        echo "BUILDSTORM_PHASE9_PERF_MATRIX outer-lifetime-invalid log=$log" >&2
        rm -f "$lifetime_lines"
        return 1
    fi
    rm -f "$lifetime_lines"
    echo "BUILDSTORM_PHASE9_PERF_MATRIX outer-lifetime-ok log=$log"
}

run_one() {
    local arch=$1
    local cpus=$2
    local round=$3
    local tag target expected_hash expected_cores log metadata overlay host_log
    local dirty_start dirty_end dirty expected_dirty

    case "$arch" in
    riscv64)
        tag=rv
        expected_hash=cba87f43ae569bcf2b8e4614f75cec1bf51bedb2804626fe466fcce3861df6f1
        ;;
    loongarch64)
        tag=la
        expected_hash=2c411447274fbd83505d2fac505a5d9e8ed8ff3bdfc3d2d6cbdb8f61ff7d90d2
        ;;
    esac
    target="final-stage9-${tag}-${cpus}c-perf"
    expected_cores=${cpus}/${cpus}

    echo "BUILDSTORM_PHASE9_PERF_MATRIX start arch=$arch cpus=$cpus round=$round/$rounds"
    make "PYTHON=conda run -n a20os --no-capture-output python" "$target"
    log=$(ls -1t \
        "$state_dir"/logs/"${arch}-buildstorm-probe-${cpus}c-stage9-perf-feedback-${commit}-"*.log |
        head -n 1)
    metadata=${log/\/logs\//\/metadata\/}
    metadata=${metadata%.log}.txt

    rg -q '^BUILDSTORM_STAGE9_PERF ok subloads=7 snapshots=8$' "$log"
    rg -q '^\[BUILDSTORM-PROBE\] summary total=1 failures=0$' "$log"
    audit_snapshots "$log"
    audit_outer_lifetime "$log"
    if rg -q 'BUILDSTORM_STAGE9_PERF failed|SIGSEGV:|code=13|code=3|Ecode=0x1[01]|lifetime_errors: [1-9]|Kernel panic|Out of memory|unhandled exception|^E: /a20-buildstorm-probe' "$log"; then
        echo "BUILDSTORM_PHASE9_PERF_MATRIX unexpected-fault log=$log" >&2
        return 1
    fi

    [[ $(metadata_value git_commit "$metadata") == "$commit" ]]
    [[ $(metadata_value official_tests_commit "$metadata") == "$official_tests_commit" ]]
    [[ $(metadata_value official_tests_dirty "$metadata") == no ]]
    [[ $(metadata_value official_image_archive_sha256 "$metadata") == "$expected_hash" ]]
    [[ $(metadata_value official_image_base_mode "$metadata") == 444 ]]
    [[ $(metadata_value official_image_base_readonly "$metadata") == yes ]]
    [[ $(metadata_value qemu_smp "$metadata") == "$cpus" ]]
    [[ $(metadata_value qemu_exit_status "$metadata") == 0 ]]
    [[ $(metadata_value qemu_timed_out "$metadata") == no ]]
    [[ $(metadata_value guest_cores "$metadata") == "$expected_cores" ]]
    [[ $(metadata_value probe_case "$metadata") == stage9-perf-feedback ]]
    [[ $(metadata_value stage9_case "$metadata") == stage9-perf-feedback ]]
    [[ $(metadata_value stage9_guest_nproc "$metadata") == "$cpus" ]]
    [[ $(metadata_value stage9_payload "$metadata") == /a20-probe/stage9-perf-probe ]]
    [[ $(metadata_value performance_lock "$metadata") == "$performance_lock" ]]
    [[ $(metadata_value performance_lock_mode "$metadata") == exclusive ]]
    [[ $(metadata_value probe_stage9_perf_sha256 "$metadata") =~ ^[0-9a-f]{64}$ ]]

    dirty_start=$(metadata_value git_dirty_start "$metadata")
    dirty_end=$(metadata_value git_dirty_end "$metadata")
    dirty=$(metadata_value git_dirty "$metadata")
    [[ "$dirty_start" == no || "$dirty_start" == yes ]]
    [[ "$dirty_end" == no || "$dirty_end" == yes ]]
    expected_dirty=no
    if [[ "$dirty_start" == yes || "$dirty_end" == yes ]]; then
        expected_dirty=yes
    fi
    [[ "$dirty" == "$expected_dirty" ]]

    host_log=$(metadata_value stage9_host_log "$metadata")
    [[ -s "$host_log" ]]
    overlay=$(metadata_value official_image_overlay "$metadata")
    [[ -n "$overlay" ]]
    echo "BUILDSTORM_PHASE9_PERF_MATRIX pass arch=$arch cpus=$cpus round=$round/$rounds overlay=$overlay log=$log"
}

for arch in riscv64 loongarch64; do
    for cpus in 1 8; do
        for ((round = 1; round <= rounds; round++)); do
            run_one "$arch" "$cpus" "$round"
        done
    done
done

pass_count=$(rg -c '^BUILDSTORM_PHASE9_PERF_MATRIX pass ' "$summary")
unique_overlays=$(rg '^BUILDSTORM_PHASE9_PERF_MATRIX pass ' "$summary" |
    sed -E 's/.* overlay=([^ ]+) log=.*/\1/' | sort -u | wc -l)
expected_passes=$(( rounds * 4 ))
if (( pass_count != expected_passes || unique_overlays != pass_count )); then
    echo "BUILDSTORM_PHASE9_PERF_MATRIX overlay-isolation-failed passes=$pass_count unique_overlays=$unique_overlays expected=$expected_passes" >&2
    exit 1
fi

elapsed_s=$(( $(date +%s) - started_at ))
echo "BUILDSTORM_PHASE9_PERF_MATRIX PASS rounds=$rounds total=$expected_passes elapsed_s=$elapsed_s"
echo "BUILDSTORM_PHASE9_PERF_MATRIX summary=$summary"
