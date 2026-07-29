#!/usr/bin/env bash
#
# Run the BuildStorm stage-4 cold-boot acceptance matrix.  Every iteration
# goes through run_final_eval.sh, so it gets a fresh official-rootfs overlay
# and its own serial log, metadata, and extracted probe result.

set -euo pipefail

rounds=${1:-20}
state_dir=${FINAL_EVAL_STATE_DIR:-.eval-state/2026}

if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-round-count]" >&2
    exit 2
fi

commit=$(git rev-parse --verify HEAD)
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
summary="$state_dir/probes/stage4-matrix-${commit}-${timestamp}.txt"
mkdir -p "$state_dir/probes"

exec > >(tee "$summary") 2>&1
echo "BUILDSTORM_STAGE4_MATRIX commit=$commit rounds=$rounds"

for arch in riscv64 loongarch64; do
    case "$arch" in
    riscv64) target_arch=rv ;;
    loongarch64) target_arch=la ;;
    esac
    for cores in 1 8; do
        target="final-stage4-${target_arch}-buildstorm-${cores}c"
        for ((round = 1; round <= rounds; round++)); do
            echo "BUILDSTORM_STAGE4_MATRIX start arch=$arch cores=$cores round=$round/$rounds"
            make "$target"
            log=$(ls -1t \
                "$state_dir"/logs/"${arch}-buildstorm-probe-${cores}c-stage4-cargo-minibuild-"*.log |
                head -n 1)
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
                exit 1
            fi
            rg -q '^qemu_exit_status=0$' "$metadata"
            rg -q '^qemu_timed_out=no$' "$metadata"
            rg -q "^guest_cores=${cores}/${cores}$" "$metadata"
            echo "BUILDSTORM_STAGE4_MATRIX pass arch=$arch cores=$cores round=$round/$rounds log=$log"
        done
    done
done

echo "BUILDSTORM_STAGE4_MATRIX PASS rounds=$rounds total=$((rounds * 4))"
echo "BUILDSTORM_STAGE4_MATRIX summary=$summary"
