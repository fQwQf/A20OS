#!/bin/bash
set -uo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
out=${1:-}
if [[ -z "$out" ]]; then
  echo "usage: $0 UNIQUE_EVIDENCE_DIRECTORY" >&2
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
if ! exec 9>"$lockdir/activity.lock"; then
  record_exit activity-lock-open 1
  exit 1
fi
flock -s 9
record_exit activity-lock-shared "$?"
if ! exec 8>"$lockdir/build-1.lock"; then
  record_exit build-lock-open 1
  exit 1
fi
if ! flock -n 8; then
  exec 8>"$lockdir/build-2.lock"
  if ! flock -n 8; then
    record_exit build-slot 75
    exit 75
  fi
fi
record_exit build-slot 0

cd "$repo" || { record_exit repository-cd 1; exit 1; }
inc=( -Ikernel/arch/x86_64/include -Ikernel/include -Ikernel \
      -Ikernel/net/lwip_port -Ikernel/external/lwip/src/include \
      -Ikernel/external/littlefs -Ikernel/external/littlefs/compat \
      -Ikernel/platform/x86_64-pc )
{
  gcc --version
  gcc -v 2>&1
  rustc -vV
} > "$out/toolchain.txt" 2>&1
df -h / > "$out/disk.leased.txt" 2>&1

gcc -std=gnu11 "${inc[@]}" -MM -MT socket_queue_harness \
  kernel/net/socket_queue.c tools/tests/socket_queue_host/harness.c \
  > "$out/dependencies.mk" 2> "$out/dependencies.log"
status=$?
record_exit dependency-scan "$status"
if [[ $status -ne 0 ]]; then exit "$status"; fi
awk '{ gsub(/\\/, ""); for (i = 1; i <= NF; i++) if ($i != "socket_queue_harness:") print $i }' \
  "$out/dependencies.mk" | xargs sha256sum > "$out/inputs.before.sha256"
status=$?
record_exit dependency-hashes "$status"
if [[ $status -ne 0 ]]; then exit "$status"; fi
sha256sum kernel/net/socket_queue.rs tools/tests/socket_queue_host/run.sh \
  >> "$out/inputs.before.sha256"

for sel in c rust; do
  cfg=()
  if [[ "$sel" == rust ]]; then cfg+=( -DCONFIG_NET_SOCKET_QUEUE_RUST ); fi
  run_stage "$sel-c-compile" gcc -std=gnu11 -Wall -Wextra \
    -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
    "${inc[@]}" "${cfg[@]}" -c kernel/net/socket_queue.c \
    -o "$out/socket_queue_$sel.o"
  status=$?
  if [[ $status -ne 0 ]]; then exit "$status"; fi

  if [[ "$sel" == rust ]]; then
    run_stage rust-object rustc --edition=2021 --crate-type lib --emit=obj \
      -C panic=abort -C overflow-checks=off kernel/net/socket_queue.rs \
      -o "$out/socket_queue_state.o"
    status=$?
    if [[ $status -ne 0 ]]; then exit "$status"; fi
  fi

  run_stage "$sel-harness-compile" gcc -std=gnu11 -Wall -Wextra \
    -Wno-builtin-declaration-mismatch -ffunction-sections -fdata-sections \
    "${inc[@]}" -c tools/tests/socket_queue_host/harness.c \
    -o "$out/harness_$sel.o"
  status=$?
  if [[ $status -ne 0 ]]; then exit "$status"; fi

  link=( gcc -Wl,--gc-sections "$out/harness_$sel.o" "$out/socket_queue_$sel.o" )
  if [[ "$sel" == rust ]]; then link+=( "$out/socket_queue_state.o" ); fi
  link+=( -o "$out/host_$sel" )
  run_stage "$sel-link" "${link[@]}"
  status=$?
  if [[ $status -ne 0 ]]; then exit "$status"; fi

  run_stage "$sel-run" "$out/host_$sel"
  status=$?
  if [[ $status -ne 0 ]]; then exit "$status"; fi
done

run_stage compare cmp "$out/c-run.log" "$out/rust-run.log"
status=$?
df -h / > "$out/disk.after.txt" 2>&1
sha256sum kernel/net/socket_queue.c kernel/net/socket_queue.rs \
  tools/tests/socket_queue_host/harness.c tools/tests/socket_queue_host/run.sh \
  > "$out/owned-sources.after.sha256"
gcc -std=gnu11 "${inc[@]}" -MM -MT socket_queue_harness \
  kernel/net/socket_queue.c tools/tests/socket_queue_host/harness.c \
  > "$out/dependencies.after.mk" 2> "$out/dependencies.after.log"
after_status=$?
record_exit dependency-scan-after "$after_status"
if [[ $after_status -ne 0 ]]; then exit "$after_status"; fi
awk '{ gsub(/\\/, ""); for (i = 1; i <= NF; i++) if ($i != "socket_queue_harness:") print $i }' \
  "$out/dependencies.after.mk" | xargs sha256sum > "$out/inputs.after.sha256"
after_status=$?
record_exit dependency-hashes-after "$after_status"
if [[ $after_status -ne 0 ]]; then exit "$after_status"; fi
sha256sum kernel/net/socket_queue.rs tools/tests/socket_queue_host/run.sh \
  >> "$out/inputs.after.sha256"
cmp "$out/inputs.before.sha256" "$out/inputs.after.sha256" \
  > "$out/input-stability.log" 2>&1
stable_status=$?
record_exit input-stability "$stable_status"
df -h / > "$out/disk.after.txt" 2>&1
sha256sum "$out/host_c" "$out/host_rust" "$out/socket_queue_c.o" \
  "$out/socket_queue_rust.o" "$out/socket_queue_state.o" > "$out/artifacts.sha256"
if [[ $status -ne 0 ]]; then exit "$status"; fi
if [[ $stable_status -ne 0 ]]; then exit "$stable_status"; fi
exit 0
