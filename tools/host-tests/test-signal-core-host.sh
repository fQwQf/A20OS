#!/bin/sh
# Signal decision core host validation.
#
#  1. Rust unit + independent-oracle vectors (rustc --test).
#  2. C<->Rust differential sweep of the production a20_signal_* ABI against an
#     independent C transcription of the frozen reference behaviour.
#  3. The shipped Python and C oracle models (tools/tests/signal_oracle) so the
#     frozen vector catalog keeps passing independently of the Rust rewrite.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-signal-core.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"

echo "== rust unit + oracle vectors =="
rustc --test --edition=2021 kernel/proc/signal_core.rs -o "$tmp_dir/signal-core-tests"
"$tmp_dir/signal-core-tests"

echo "== C<->Rust differential sweep =="
# The kernel source is no_std; host-link it with std by dropping the attribute.
sed '1{/^#!\[no_std\]$/d}' kernel/proc/signal_core.rs > "$tmp_dir/signal_core_host.rs"
rustc --edition=2021 --crate-type staticlib -C opt-level=2 \
    -o "$tmp_dir/libsignal_core.a" "$tmp_dir/signal_core_host.rs"
cc -O2 -std=gnu11 -Wall -Wextra \
    tools/tests/signal_oracle/diff_harness.c "$tmp_dir/libsignal_core.a" \
    -lpthread -ldl -lm -o "$tmp_dir/signal-diff"
"$tmp_dir/signal-diff"

echo "== shipped oracle models (python + C) =="
if command -v python3 >/dev/null 2>&1; then
    python3 tools/tests/signal_oracle/model_py.py "$tmp_dir/obs_py.tsv"
    python3 tools/tests/signal_oracle/emit_c.py "$tmp_dir/vectors_gen.c"
    cc -O2 -std=gnu11 -w -I "$tmp_dir" -o "$tmp_dir/model-c" \
        tools/tests/signal_oracle/model_c.c
    "$tmp_dir/model-c" > "$tmp_dir/obs_c.tsv"
    python3 tools/tests/signal_oracle/compare.py "$tmp_dir/obs_py.tsv" "$tmp_dir/obs_c.tsv"
else
    echo "python3 missing; skipped oracle models"
fi
