#!/usr/bin/env bash
#
# Run one ordered Stage-7 full-build level for both architectures.  Full
# builds are deliberately serial: the default level is also bounded by the
# platform's 3000-second outer deadline for each complete runner invocation.

set -euo pipefail

level=${1:-}
rounds=${2:-}
state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}

case "$level" in
1c-j1)
    guest_cpus=1
    probe_case=stage7-full-j1
    jobs=1
    per_run_budget_s=28800
    ;;
8c-j1)
    guest_cpus=8
    probe_case=stage7-full-j1
    jobs=1
    per_run_budget_s=28800
    ;;
8c-j2)
    guest_cpus=8
    probe_case=stage7-full-j2
    jobs=2
    per_run_budget_s=28800
    ;;
8c-j4)
    guest_cpus=8
    probe_case=stage7-full-j4
    jobs=4
    per_run_budget_s=28800
    ;;
8c-j8)
    guest_cpus=8
    probe_case=stage7-full-j8
    jobs=8
    per_run_budget_s=28800
    ;;
8c-default)
    guest_cpus=8
    probe_case=stage7-full-default
    jobs=default
    per_run_budget_s=3000
    ;;
*)
    echo "usage: $0 1c-j1|8c-j1|8c-j2|8c-j4|8c-j8|8c-default [rounds]" >&2
    exit 2
    ;;
esac

if [[ -z "$rounds" ]]; then
    if [[ "$level" == 8c-default ]]; then
        rounds=2
    else
        rounds=1
    fi
fi
if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    echo "rounds must be a positive integer" >&2
    exit 2
fi

default_matrix_budget_s=$(( per_run_budget_s * rounds * 2 + 3600 ))
matrix_budget_s=${BUILDSTORM_MATRIX_BUDGET_SECONDS:-$default_matrix_budget_s}
if [[ ! "$matrix_budget_s" =~ ^[1-9][0-9]*$ ]]; then
    echo "BUILDSTORM_MATRIX_BUDGET_SECONDS must be positive" >&2
    exit 2
fi

commit=$(git rev-parse --verify HEAD)
official_tests_commit=$(git -C contest/testsuits-for-oskernel rev-parse --verify HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
summary=${BUILDSTORM_MATRIX_SUMMARY:-"$state_dir/probes/stage7-${level}-matrix-${commit}-${timestamp}.txt"}
mkdir -p "$state_dir/probes"

if [[ ${BUILDSTORM_MATRIX_INTERNAL:-0} != 1 ]]; then
    export BUILDSTORM_MATRIX_INTERNAL=1
    export BUILDSTORM_MATRIX_SUMMARY="$summary"
    set +e
    timeout --signal=TERM --kill-after=30 "$matrix_budget_s" \
        bash "$0" "$level" "$rounds"
    status=$?
    set -e
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "BUILDSTORM_STAGE7_MATRIX TIME_BUDGET_EXCEEDED level=$level budget_s=$matrix_budget_s" |
            tee -a "$summary" >&2
    fi
    exit "$status"
fi

exec > >(tee "$summary") 2>&1
matrix_work_dir=$(mktemp -d /tmp/a20os-buildstorm-stage7.XXXXXX)
export FINAL_EVAL_IMAGE_CACHE_DIR=${FINAL_EVAL_IMAGE_CACHE_DIR:-/tmp/a20os-buildstorm-matrix-cache}
export FINAL_EVAL_WORK_DIR="$matrix_work_dir"
cleanup() {
    rm -rf -- "$matrix_work_dir"
}
trap cleanup EXIT

echo "BUILDSTORM_STAGE7_MATRIX commit=$commit level=$level rounds=$rounds budget_s=$matrix_budget_s per_run_budget_s=$per_run_budget_s"
echo "BUILDSTORM_STAGE7_MATRIX official_tests_commit=$official_tests_commit"
echo "BUILDSTORM_STAGE7_MATRIX cache=$FINAL_EVAL_IMAGE_CACHE_DIR work=$FINAL_EVAL_WORK_DIR"
started_at=$(date +%s)

exact_lifetime_fields='^(task_objects|task_refs|listed_tasks|listed_refs|pid_entries|runqueue_entries|dispatching_tasks|cpu_owned_tasks|wait_entries|wake_entries|timeout_entries|open_fds|vfile_objects|page_cache_dirty|page_cache_pinned|zombies|lifetime_errors):'

metadata_value() {
    local key=$1
    local metadata=$2
    sed -n "s/^${key}=//p" "$metadata" | tail -n 1
}

audit_lifetime() {
    local log=$1
    local before after lifetime_lines vnode_before vnode_after
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
        echo "BUILDSTORM_STAGE7_MATRIX resource-baseline-mismatch log=$log" >&2
        diff -u "$before" "$after" >&2 || true
        rm -f "$before" "$after"
        return 1
    fi
    rm -f "$before" "$after"
    vnode_before=$(awk '/^vnode_objects:/{print $2; exit}' "$log")
    vnode_after=$(awk '/^vnode_objects:/{value=$2} END{print value}' "$log")
    if (( vnode_after > vnode_before )); then
        echo "BUILDSTORM_STAGE7_MATRIX vnode-growth before=$vnode_before after=$vnode_after log=$log" >&2
        return 1
    fi
}

run_one() {
    local arch=$1
    local round=$2
    local tag target expected_hash expected_helper_size expected_target expected_cores
    local log metadata host_log overlay helper_before helper_after artifact_bytes
    local compile_elapsed start_uptime end_uptime runner_elapsed

    case "$arch" in
    riscv64)
        tag=rv
        expected_hash=cba87f43ae569bcf2b8e4614f75cec1bf51bedb2804626fe466fcce3861df6f1
        expected_helper_size=277827240
        expected_target=/work/tgoskits/target/riscv64gc-unknown-linux-musl
        ;;
    loongarch64)
        tag=la
        expected_hash=2c411447274fbd83505d2fac505a5d9e8ed8ff3bdfc3d2d6cbdb8f61ff7d90d2
        expected_helper_size=280919472
        expected_target=/work/tgoskits/target/loongarch64-unknown-linux-musl
        ;;
    esac
    expected_cores=${guest_cpus}/${guest_cpus}
    target="final-stage7-${tag}-${level}"

    echo "BUILDSTORM_STAGE7_MATRIX start arch=$arch level=$level round=$round/$rounds"
    set +e
    timeout --signal=TERM --kill-after=30 "$per_run_budget_s" \
        make "PYTHON=conda run -n a20os --no-capture-output python" "$target"
    status=$?
    set -e
    if [[ "$status" -ne 0 ]]; then
        echo "BUILDSTORM_STAGE7_MATRIX runner-failed arch=$arch level=$level round=$round status=$status" >&2
        return 1
    fi

    log=$(ls -1t \
        "$state_dir"/logs/"${arch}-buildstorm-probe-${guest_cpus}c-${probe_case}-${commit}-"*.log |
        head -n 1)
    metadata=${log/\/logs\//\/metadata\/}
    metadata=${metadata%.log}.txt

    rg -q '^BUILDSTORM_TOOLCHAIN ok$' "$log"
    rg -q '^BUILDSTORM_MINIBUILD ok$' "$log"
    rg -q '^BUILDSTORM_STAGE7_COMPILE ok$' "$log"
    rg -q "^BUILDSTORM_COMPILE mode=stage7 ok=true .* cores=${guest_cpus} jobs=${jobs} bytes=[0-9]+ arch=${arch}$" "$log"
    rg -q '^\[BUILDSTORM-PROBE\] summary total=1 failures=0$' "$log"
    if rg -q 'SIGSEGV:|code=13|code=3|Ecode=0x1[01]|lifetime_errors: [1-9]|Kernel panic|Out of memory|unhandled exception|^E: /a20-buildstorm-probe|BUILDSTORM_COMPILE mode=stage7 ok=false' "$log"; then
        echo "BUILDSTORM_STAGE7_MATRIX unexpected-fault log=$log" >&2
        return 1
    fi
    audit_lifetime "$log"

    [[ $(metadata_value git_commit "$metadata") == "$commit" ]]
    [[ $(metadata_value git_dirty "$metadata") == no ]]
    [[ $(metadata_value official_tests_commit "$metadata") == "$official_tests_commit" ]]
    [[ $(metadata_value official_tests_dirty "$metadata") == no ]]
    [[ $(metadata_value official_image_archive_sha256 "$metadata") == "$expected_hash" ]]
    [[ $(metadata_value official_image_base_mode "$metadata") == 444 ]]
    [[ $(metadata_value official_image_base_readonly "$metadata") == yes ]]
    [[ $(metadata_value qemu_smp "$metadata") == "$guest_cpus" ]]
    [[ $(metadata_value qemu_timeout_s "$metadata") == "$per_run_budget_s" ]]
    [[ $(metadata_value qemu_exit_status "$metadata") == 0 ]]
    [[ $(metadata_value qemu_timed_out "$metadata") == no ]]
    [[ $(metadata_value guest_cores "$metadata") == "$expected_cores" ]]

    [[ $(metadata_value stage7_case "$metadata") == "$probe_case" ]]
    [[ $(metadata_value stage7_arch "$metadata") == "$arch" ]]
    [[ $(metadata_value stage7_cargo_jobs "$metadata") == "$jobs" ]]
    [[ $(metadata_value stage7_guest_nproc "$metadata") == "$guest_cpus" ]]
    [[ $(metadata_value stage7_compile_command "$metadata") == cargo_xtask_arceos_build ]]
    [[ $(metadata_value stage7_target_cleanup "$metadata") == "$expected_target" ]]
    [[ $(metadata_value stage7_helper_cache_fingerprint "$metadata") == present ]]
    [[ $(metadata_value stage7_helper_cache_deps "$metadata") == present ]]
    [[ $(metadata_value stage7_helper_cache_build "$metadata") == present ]]
    [[ $(metadata_value stage7_helper_before_mode "$metadata") == 755 ]]
    [[ $(metadata_value stage7_helper_before_executable "$metadata") == yes ]]
    [[ $(metadata_value stage7_helper_build_rc "$metadata") == 0 ]]
    [[ $(metadata_value stage7_helper_after_mode "$metadata") == 755 ]]
    [[ $(metadata_value stage7_helper_after_executable "$metadata") == yes ]]
    [[ $(metadata_value stage7_compile_rc "$metadata") == 0 ]]
    [[ $(metadata_value stage7_uptime_monotonic "$metadata") == yes ]]

    helper_before=$(metadata_value stage7_helper_before_bytes "$metadata")
    helper_after=$(metadata_value stage7_helper_after_bytes "$metadata")
    [[ "$helper_before" =~ ^[1-9][0-9]*$ && "$helper_after" =~ ^[1-9][0-9]*$ ]]
    (( helper_before == expected_helper_size && helper_after > 0 ))
    artifact_bytes=$(metadata_value stage7_artifact_bytes "$metadata")
    [[ "$artifact_bytes" =~ ^[0-9]+$ ]]
    (( artifact_bytes >= 500000 ))
    [[ $(metadata_value stage7_artifact_sha256 "$metadata") =~ ^[0-9a-f]{64}$ ]]
    compile_elapsed=$(metadata_value stage7_compile_elapsed_s "$metadata")
    [[ "$compile_elapsed" =~ ^[0-9]+([.][0-9]+)?$ ]]
    start_uptime=$(metadata_value stage7_compile_start_uptime_s "$metadata")
    end_uptime=$(metadata_value stage7_compile_end_uptime_s "$metadata")
    awk "BEGIN{exit !((\"$end_uptime\"+0) >= (\"$start_uptime\"+0))}"
    runner_elapsed=$(metadata_value runner_wall_elapsed_s "$metadata")
    [[ "$runner_elapsed" =~ ^[0-9]+$ ]]
    if [[ "$level" == 8c-default ]]; then
        (( runner_elapsed < 3000 ))
    fi

    host_log=$(metadata_value stage7_build_host_log "$metadata")
    [[ -s "$host_log" ]]
    rg -q '^----- stage7 compile stdout/stderr begin -----$' "$host_log"
    rg -q '^----- stage7 compile stdout/stderr end -----$' "$host_log"
    overlay=$(metadata_value official_image_overlay "$metadata")
    [[ -n "$overlay" ]]
    echo "BUILDSTORM_STAGE7_MATRIX pass arch=$arch level=$level round=$round/$rounds compile_elapsed_s=$compile_elapsed runner_elapsed_s=$runner_elapsed bytes=$artifact_bytes overlay=$overlay log=$log"
}

for arch in riscv64 loongarch64; do
    for ((round = 1; round <= rounds; round++)); do
        run_one "$arch" "$round"
    done
done

pass_count=$(rg -c '^BUILDSTORM_STAGE7_MATRIX pass ' "$summary")
unique_overlays=$(rg '^BUILDSTORM_STAGE7_MATRIX pass ' "$summary" |
    sed -E 's/.* overlay=([^ ]+) log=.*/\1/' | sort -u | wc -l)
expected_passes=$(( rounds * 2 ))
if (( pass_count != expected_passes || unique_overlays != pass_count )); then
    echo "BUILDSTORM_STAGE7_MATRIX overlay-isolation-failed passes=$pass_count unique_overlays=$unique_overlays expected=$expected_passes" >&2
    exit 1
fi

elapsed_s=$(( $(date +%s) - started_at ))
echo "BUILDSTORM_STAGE7_MATRIX PASS level=$level rounds=$rounds total=$expected_passes elapsed_s=$elapsed_s"
echo "BUILDSTORM_STAGE7_MATRIX summary=$summary"
