#!/bin/bash
# Host regression evidence for the IPv4 filter state machine.
#
# The C reference was retired with the Rust takeover, so the run is a golden-
# transcript check: the exact production kernel/net/netfilter.c and
# kernel/net/netfilter_filter.rs are compiled once, the same scenarios are
# driven through the production entry points, and the transcript must match
# tools/tests/netfilter_filter_host/golden.txt byte for byte.  The golden was
# recorded while both implementations existed and their transcripts were
# byte-identical, so it is the C behaviour frozen as an expectation -- not a copy
# of the C code.
#
# The harness-side -DCONFIG_NETFILTER_FILTER_RUST only selects which conntrack
# stub it defines; the production file has no guards left.
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
      -Ikernel/include -Ikernel -Ikernel/net/lwip_port
      -Ikernel/external/lwip/src/include -Ikernel/external/littlefs
      -Ikernel/external/littlefs/compat -Ikernel/platform/x86_64-pc
      -DARCH_TIMER_FREQ=1000 )

owned_sources=( kernel/net/netfilter.c kernel/net/netfilter_filter.rs
                kernel/include/net/netfilter.h
                kernel/include/net/netfilter_filter_state.h
                kernel/rust/runtime.rs
                tools/tests/netfilter_filter_host/harness.c
                tools/tests/netfilter_filter_host/golden.txt
                tools/tests/netfilter_filter_host/run.sh )

hash_inputs() {
  sha256sum "${owned_sources[@]}" > "$1"
  gcc -std=gnu11 "${inc[@]}" -MM -MT filter_harness \
    kernel/net/netfilter.c tools/tests/netfilter_filter_host/harness.c \
    > "$out/dependencies.mk" 2> "$out/dependencies.log"
  awk '{ gsub(/\\/, ""); for (i = 1; i <= NF; i++) if ($i != "filter_harness:") print $i }' \
    "$out/dependencies.mk" | xargs sha256sum >> "$1"
}

hash_inputs "$out/inputs.before.sha256"
status=$?
record_exit input-hashes "$status"
if [[ $status -ne 0 ]]; then exit "$status"; fi

run_stage compile gcc -std=gnu11 -O1 -Wall -Wextra -Werror -pthread \
  -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
  "${inc[@]}" -c kernel/net/netfilter.c -o "$out/netfilter.o"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-object rustc --edition=2021 --crate-type lib --emit=obj \
  -C panic=abort -C overflow-checks=off \
  kernel/net/netfilter_filter.rs -o "$out/netfilter_filter_state.o"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-runtime rustc --edition=2021 --crate-type staticlib \
  -C panic=abort -C overflow-checks=off \
  kernel/rust/runtime.rs -o "$out/liba20_rust_runtime.a"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-unit-tests rustc --edition=2021 --test \
  kernel/net/netfilter_filter.rs -o "$out/filter_unit_tests"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage rust-unit-run "$out/filter_unit_tests" --test-threads=1
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage harness-compile gcc -std=gnu11 -O1 -Wall -Wextra -pthread \
  -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
  "${inc[@]}" -DCONFIG_NETFILTER_FILTER_RUST \
  -c tools/tests/netfilter_filter_host/harness.c -o "$out/harness.o"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage link gcc -Wl,--gc-sections -pthread "$out/harness.o" "$out/netfilter.o" \
  "$out/netfilter_filter_state.o" "$out/liba20_rust_runtime.a" -o "$out/host"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage run "$out/host"
status=$?; if [[ $status -ne 0 ]]; then exit "$status"; fi
run_stage golden-compare cmp tools/tests/netfilter_filter_host/golden.txt \
  "$out/run.log"
status=$?

hash_inputs "$out/inputs.after.sha256"
after_status=$?
record_exit input-hashes-after "$after_status"
cmp "$out/inputs.before.sha256" "$out/inputs.after.sha256" > "$out/input-stability.log" 2>&1
stable_status=$?
record_exit input-stability "$stable_status"
sha256sum "$out/host" "$out/netfilter.o" "$out/netfilter_filter_state.o" \
  > "$out/artifacts.sha256"
if [[ $status -ne 0 ]]; then exit "$status"; fi
if [[ $after_status -ne 0 ]]; then exit "$after_status"; fi
if [[ $stable_status -ne 0 ]]; then exit "$stable_status"; fi
exit 0
