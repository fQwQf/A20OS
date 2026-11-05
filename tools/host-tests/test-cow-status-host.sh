#!/bin/sh
# Host differential harness for kernel/mm/cow_status.rs.  Guest coverage is
# separate: mm_stress's fork-COW phases must keep cow_from_status non-zero.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-cow-status.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/cow_status.rs" -o "$tmp/cow_status.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_cow_status.c" \
    "$tmp/cow_status.o" -o "$tmp/test-cow-status"
"$tmp/test-cow-status"
