#!/bin/sh
# Host differential harness for kernel/mm/swap_slots.rs.  Guest coverage is
# separate: mm_stress and oom_stress swap real pages.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-swap-slots.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/swap_slots.rs" -o "$tmp/swap_slots.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_swap_slots.c" \
    "$tmp/swap_slots.o" -o "$tmp/test-swap-slots"
"$tmp/test-swap-slots"
