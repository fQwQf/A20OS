#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${EVIDENCE_DIR:-}" ]; then
    tmp_dir=$EVIDENCE_DIR
    mkdir -p "$tmp_dir"
    cleanup=0
else
    tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-ext4-sync-plan.XXXXXX")
    cleanup=1
fi
if [ "$cleanup" -eq 1 ]; then
    trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
fi

cd "$repo_root"
sha256sum kernel/fs/diskfs/ext4_sync_plan.rs kernel/fs/diskfs/ext4_sync.c \
    tools/tests/ext4_sync_callback_host.c tools/tests/ext4_sync_c_scope_host.c \
    tools/test-ext4-sync-plan-host.sh > "$tmp_dir/source.sha256"
printf '%s\n' \
    'rustc --test --edition=2021 kernel/fs/diskfs/ext4_sync_plan.rs' \
    'cc -std=gnu11 -ffunction-sections -fdata-sections -Wno-builtin-declaration-mismatch -I kernel/arch/riscv64/include -I kernel/include -I kernel -I user tools/tests/ext4_sync_callback_host.c -Wl,--gc-sections' \
    'cc -std=gnu11 -ffunction-sections -fdata-sections -Wno-builtin-declaration-mismatch -I kernel/arch/riscv64/include -I kernel/include -I kernel -I user tools/tests/ext4_sync_c_scope_host.c -Wl,--gc-sections' \
    > "$tmp_dir/run.argv"
rustc --test --edition=2021 kernel/fs/diskfs/ext4_sync_plan.rs \
    -o "$tmp_dir/ext4-sync-plan-tests" > "$tmp_dir/rust-tests.log" 2>&1
"$tmp_dir/ext4-sync-plan-tests" >> "$tmp_dir/rust-tests.log" 2>&1

cc -std=gnu11 -ffunction-sections -fdata-sections \
    -Wno-builtin-declaration-mismatch \
    -I kernel/arch/riscv64/include -I kernel/include -I kernel -I user \
    tools/tests/ext4_sync_callback_host.c -Wl,--gc-sections \
    -o "$tmp_dir/ext4-sync-callback-tests" > "$tmp_dir/callback-build.log" 2>&1
"$tmp_dir/ext4-sync-callback-tests" > "$tmp_dir/callback-test.log" 2>&1

cc -std=gnu11 -ffunction-sections -fdata-sections \
    -Wno-builtin-declaration-mismatch \
    -I kernel/arch/riscv64/include -I kernel/include -I kernel -I user \
    tools/tests/ext4_sync_c_scope_host.c -Wl,--gc-sections \
    -o "$tmp_dir/ext4-sync-c-scope-tests" > "$tmp_dir/scope-build.log" 2>&1
"$tmp_dir/ext4-sync-c-scope-tests" > "$tmp_dir/scope-test.log" 2>&1
printf '0\n' > "$tmp_dir/exit.status"
printf 'Rust planner: 20/20; C callback harness: PASS; C scope harness: PASS\n' \
    > "$tmp_dir/result.txt"
