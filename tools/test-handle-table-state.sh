#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp_root=${HANDLE_TABLE_STATE_TEST_DIR:-${TMPDIR:-/tmp}}
build_dir=$(mktemp -d "${tmp_root%/}/a20-handle-table-state.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT

rustc --edition=2021 --crate-type=lib --emit=obj -C opt-level=2 -C panic=abort \
    "$repo_root/kernel/abi/native/handle_table_state.rs" \
    -o "$build_dir/handle_table_state.o"
rustc --edition=2021 --crate-type=lib --emit=obj -C opt-level=2 -C panic=abort \
    "$repo_root/kernel/abi/native/handle_slot.rs" \
    -o "$build_dir/handle_slot.o"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -g -I"$repo_root" \
    "$repo_root/tools/test_handle_table_state.c" \
    "$build_dir/handle_table_state.o" "$build_dir/handle_slot.o" \
    -o "$build_dir/test-handle-table-state"
"$build_dir/test-handle-table-state"
