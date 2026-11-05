#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a20-rt-throttle.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$repo/kernel/arch/riscv64/include" \
    -I"$repo/kernel/include" \
    "$repo/tools/tests/rt_throttle_accounting.c" \
    -o "$tmp/rt-throttle-accounting"
"$tmp/rt-throttle-accounting"
echo "RT throttle counter transition PASS"
