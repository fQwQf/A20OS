#!/bin/sh
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-uxfs-flush.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cd "$repo_root"
cc -std=gnu11 -ffunction-sections -fdata-sections -fno-builtin \
    -Iuser -Iuser/liba20rt -Iuser/svc -Ikernel/include \
    -c user/svc/ufsd_rust_abi.c -o "$tmp_dir/ufsd-abi.o"
cc -std=gnu11 -Iuser/liba20rt -c tools/tests/test_uxfs_flush_client.c \
    -o "$tmp_dir/client-test.o"
cc "$tmp_dir/client-test.o" "$tmp_dir/ufsd-abi.o" \
    -Wl,--gc-sections -o "$tmp_dir/flush-abi-test"
"$tmp_dir/flush-abi-test"
echo "UXFS production client + v1/v2 mock ABI: PASS"
