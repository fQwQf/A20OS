#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-readiness-filter.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cd "$repo_root"

cc -std=gnu11 -fno-builtin -ffunction-sections -fdata-sections \
    -D__KERNEL__ -Ikernel/include -Ikernel/arch/riscv64/include \
    tools/test-readiness-filter-host.c -Wl,--gc-sections \
    -o "$tmp_dir/test-readiness-filter-host"
"$tmp_dir/test-readiness-filter-host"
