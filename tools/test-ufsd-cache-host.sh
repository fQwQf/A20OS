#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-ufsd-cache.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"
rustc --test --edition=2021 user/svc/fscompat/bcache_user.rs \
    -o "$tmp_dir/ufsd-cache-tests"
"$tmp_dir/ufsd-cache-tests"

# Exercise the production C adapter itself: only a NULL callback receives the
# private absent sentinel; callback-returned errno values must survive intact.
cc -std=gnu11 -D_CORE_SYNC_H -Ikernel/include \
    -Ikernel/arch/riscv64/include -Iuser/svc/fscompat \
    -ffunction-sections -fdata-sections -c \
    user/svc/fscompat/bcache_user_rust_abi.c -o "$tmp_dir/adapter.o"
cc -std=gnu11 -Ikernel/include tools/tests/test_bcache_flush_adapter.c \
    "$tmp_dir/adapter.o" -Wl,--gc-sections -o "$tmp_dir/adapter-tests"
"$tmp_dir/adapter-tests"
echo "bcache flush adapter: NULL/-95/-5/success PASS"
