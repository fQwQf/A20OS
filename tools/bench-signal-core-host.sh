#!/bin/sh
# Paired C-vs-Rust microbenchmark for the signal decision core.
#
# Builds the production Rust core as a host staticlib (-O2) and the C reference
# transcription at the same optimisation, then runs plain and pinned rounds with
# alternating order. Fixed CPU/optimisation for both sides. Requires the host
# compilers; no target runtime.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-signal-bench.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cd "$repo_root"

sed '1{/^#!\[no_std\]$/d}' kernel/proc/signal_core.rs > "$tmp_dir/signal_core_host.rs"
rustc --edition=2021 --crate-type staticlib -C opt-level=2 \
    -o "$tmp_dir/libsignal_core.a" "$tmp_dir/signal_core_host.rs"
cc -O2 -std=gnu11 -Wall -Wextra \
    tools/tests/signal_oracle/bench.c "$tmp_dir/libsignal_core.a" \
    -lpthread -ldl -lm -o "$tmp_dir/signal-bench"

rounds=${1:-12}
echo "== unpinned =="
"$tmp_dir/signal-bench" "$rounds"
if command -v taskset >/dev/null 2>&1; then
    cpu=$(nproc)
    cpu=$((cpu > 1 ? cpu - 1 : 0))
    echo "== pinned cpu $cpu =="
    taskset -c "$cpu" "$tmp_dir/signal-bench" "$rounds"
fi
