#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-ufsd-rust.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"
rustc --test --edition=2021 user/svc/ufsd_protocol.rs \
    -o "$tmp_dir/ufsd-protocol-tests"
"$tmp_dir/ufsd-protocol-tests"

rustc --test --edition=2021 user/svc/ufs_lifetime.rs \
    -o "$tmp_dir/ufsd-lifetime-tests"
"$tmp_dir/ufsd-lifetime-tests"

rustc --test --edition=2021 user/svc/ext4_orphan.rs \
    -o "$tmp_dir/ext4-orphan-tests"
"$tmp_dir/ext4-orphan-tests"

rustc --test --edition=2021 user/svc/ufsd_supervision.rs \
    -o "$tmp_dir/ufsd-supervision-tests"
"$tmp_dir/ufsd-supervision-tests"

rustc --test --edition=2021 kernel/fs/diskfs/ext4_mount_state.rs \
    -o "$tmp_dir/ext4-mount-state-tests"
"$tmp_dir/ext4-mount-state-tests"

rustc --test --edition=2021 kernel/fs/diskfs/ext4_sync_plan.rs \
    -o "$tmp_dir/ext4-sync-plan-tests"
"$tmp_dir/ext4-sync-plan-tests"

cc -std=gnu11 -ffunction-sections -fdata-sections \
    -Wno-builtin-declaration-mismatch \
    -I kernel/arch/riscv64/include -I kernel/include -I kernel -I user \
    tools/tests/ext4_sync_callback_host.c -Wl,--gc-sections \
    -o "$tmp_dir/ext4-sync-callback-tests"
"$tmp_dir/ext4-sync-callback-tests"
