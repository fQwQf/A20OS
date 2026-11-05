#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-ufsd-bcache-c.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"
cc -std=gnu11 -Iuser/svc/fscompat -Ikernel/include \
    -Ikernel/arch/riscv64/include -ffunction-sections -fdata-sections \
    -c user/svc/fscompat/bcache_user.c -o "$tmp_dir/bcache_user.o"
cc -std=gnu11 -Iuser/svc/fscompat -Ikernel/include \
    -Ikernel/arch/riscv64/include tools/tests/test_bcache_user_flush.c \
    "$tmp_dir/bcache_user.o" -Wl,--gc-sections -o "$tmp_dir/test"
"$tmp_dir/test"
echo "C userspace bcache device/flush contract: PASS"
