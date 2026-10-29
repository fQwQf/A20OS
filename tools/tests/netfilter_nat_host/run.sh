#!/bin/bash
# Host regression evidence for the conntrack/NAT state machine.
#
# Production C storage/rewrite adapters and Rust state machines are linked
# together. Explicit assertions cover collision allocation, stable reply owners,
# forwarded ingress/egress, exhaustion without packet mutation, and four-thread
# contention. Goldens specify intentional behavior, including refusing live
# eviction and correcting reverse translation; they no longer freeze C bugs.
# Default and embedded capacities are checked, plus a 2 GHz timer build that
# catches narrowing raw ticks or multiplying before division in timeout code.
#
# Inputs are hashed before and after, so the comparison cannot be made across two
# source revisions by accident.
set -uo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
out=${1:-}
if [[ -z "$out" ]]; then
  echo "usage: $0 UNIQUE_EVIDENCE_DIRECTORY" >&2
  exit 64
fi
if [[ "$out" != /* ]]; then
  echo "usage: $0 ABSOLUTE_EVIDENCE_DIRECTORY" >&2
  exit 64
fi
mkdir -p "$(dirname -- "$out")"
if ! mkdir "$out"; then
  echo "evidence directory already exists or cannot be created: $out" >&2
  exit 73
fi
printf '%q ' "$0" "$@" > "$out/run.argv"
printf '\n' >> "$out/run.argv"

common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir) || exit 74
lockdir="$common/parallel-rewrite"

record_exit() { printf '%s %s\n' "$1" "$2" >> "$out/exits.txt"; }
run_stage() {
  local label=$1
  shift
  printf '%q ' "$@" > "$out/$label.argv"
  printf '\n' >> "$out/$label.argv"
  "$@" > "$out/$label.log" 2>&1
  local status=$?
  record_exit "$label" "$status"
  return "$status"
}

df -h / > "$out/disk.before.txt" 2>&1
exec 9>"$lockdir/activity.lock" || exit 1
flock -s 9
exec 8>"$lockdir/build-1.lock" || exit 1
if ! flock -n 8; then
  exec 8>"$lockdir/build-2.lock"
  if ! flock -n 8; then
    record_exit build-slot 75
    exit 75
  fi
fi
record_exit build-slot 0

cd "$repo" || { record_exit repository-cd 1; exit 1; }

inc=( -Itools/tests/socket_registry_host/hostinc -Ikernel/arch/x86_64/include
      -Ikernel/include -Ikernel -Ikernel/net/backends/lwip/port
      -Ikernel/net/backends/native/protocol/src/include -Ikernel/external/littlefs
      -Ikernel/external/littlefs/compat -Ikernel/arch/x86_64/boards/x86_64-pc
      )

{
  gcc --version
  rustc -vV
} > "$out/toolchain.txt" 2>&1
df -h / > "$out/disk.leased.txt" 2>&1

owned_sources=( kernel/net/filter/netfilter_nat.c kernel/net/filter/netfilter_nat.rs
                kernel/net/filter/netfilter_rewrite.c kernel/net/filter/netfilter_rewrite.rs
                kernel/include/net/netfilter.h kernel/include/net/netfilter_nat_state.h
                kernel/include/net/netfilter_rewrite.h
                kernel/rust/runtime.rs
                tools/tests/netfilter_nat_host/harness.c
                tools/tests/netfilter_nat_host/golden.txt
                tools/tests/netfilter_nat_host/golden-embedded.txt
                tools/tests/netfilter_nat_host/run.sh )

hash_inputs() {
  sha256sum "${owned_sources[@]}" > "$1"
  gcc -std=gnu11 "${inc[@]}" -MM -MT nat_harness \
    kernel/net/filter/netfilter_nat.c kernel/net/filter/netfilter_rewrite.c \
    tools/tests/netfilter_nat_host/harness.c \
    > "$out/dependencies.mk" 2> "$out/dependencies.log"
  awk '{ gsub(/\\/, ""); for (i = 1; i <= NF; i++) if ($i != "nat_harness:") print $i }' \
    "$out/dependencies.mk" | xargs sha256sum >> "$1"
}

hash_inputs "$out/inputs.before.sha256"
status=$?
record_exit input-hashes "$status"
if [[ $status -ne 0 ]]; then exit "$status"; fi

run_stage rust-nat-object rustc --edition=2021 --crate-type lib --emit=obj \
  -C panic=abort -C overflow-checks=off \
  kernel/net/filter/netfilter_nat.rs -o "$out/netfilter_nat_state.o"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-rewrite-object rustc --edition=2021 --crate-type lib --emit=obj \
  -C panic=abort -C overflow-checks=off \
  kernel/net/filter/netfilter_rewrite.rs -o "$out/netfilter_rewrite_state.o"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-runtime rustc --edition=2021 --crate-type staticlib \
  -C panic=abort -C overflow-checks=off \
  kernel/rust/runtime.rs -o "$out/liba20_rust_runtime.a"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-unit-tests rustc --edition=2021 --test \
  kernel/net/filter/netfilter_nat.rs -o "$out/nat_unit_tests"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-unit-run "$out/nat_unit_tests" --test-threads=1
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi

for geom in default embedded highclock; do
  cfg=( -DARCH_TIMER_FREQ=1000 )
  golden=tools/tests/netfilter_nat_host/golden.txt
  tag=$geom
  if [[ "$geom" == embedded ]]; then
    cfg+=( -DCONFIG_NET_PROFILE=1 )
    golden=tools/tests/netfilter_nat_host/golden-embedded.txt
    tag=emb
  fi
  if [[ "$geom" == highclock ]]; then
    cfg=( -DARCH_TIMER_FREQ=2000000000 )
  fi
  run_stage "$tag-nat-compile" gcc -pthread -std=gnu11 -O1 -Wall -Wextra -Werror \
    -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
    "${inc[@]}" "${cfg[@]}" \
    -c kernel/net/filter/netfilter_nat.c -o "$out/${tag}_nat.o"
  status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
  run_stage "$tag-rewrite-compile" gcc -pthread -std=gnu11 -O1 -Wall -Wextra -Werror \
    -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
    "${inc[@]}" "${cfg[@]}" \
    -c kernel/net/filter/netfilter_rewrite.c -o "$out/${tag}_rewrite.o"
  status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
  run_stage "$tag-harness-compile" gcc -pthread -std=gnu11 -O1 -Wall -Wextra \
    -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
    "${inc[@]}" "${cfg[@]}" \
    -c tools/tests/netfilter_nat_host/harness.c -o "$out/${tag}_harness.o"
  status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
  run_stage "$tag-link" gcc -pthread -Wl,--gc-sections "$out/${tag}_harness.o" \
    "$out/${tag}_nat.o" "$out/${tag}_rewrite.o" \
    "$out/netfilter_nat_state.o" "$out/netfilter_rewrite_state.o" \
    "$out/liba20_rust_runtime.a" -o "$out/${tag}_host"
  status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
  run_stage "$tag-run" "$out/${tag}_host"
  status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
  run_stage "$tag-golden-compare" cmp "$golden" "$out/${tag}-run.log"
  status=$?
done

hash_inputs "$out/inputs.after.sha256"
after_status=$?
record_exit input-hashes-after "$after_status"
cmp "$out/inputs.before.sha256" "$out/inputs.after.sha256" \
  > "$out/input-stability.log" 2>&1
stable_status=$?
record_exit input-stability "$stable_status"
df -h / > "$out/disk.after.txt" 2>&1
sha256sum "$out/default_host" "$out/emb_host" "$out/default_nat.o" \
  "$out/emb_nat.o" "$out/netfilter_nat_state.o" > "$out/artifacts.sha256"
if [[ $status -ne 0 ]]; then exit "$status"; fi
if [[ $after_status -ne 0 ]]; then exit "$after_status"; fi
if [[ $stable_status -ne 0 ]]; then exit "$stable_status"; fi
exit 0
