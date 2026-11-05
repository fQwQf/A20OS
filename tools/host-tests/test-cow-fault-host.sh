#!/bin/sh
# Host differential harness for kernel/mm/cow_fault.rs.  Guest coverage is
# separate: mm_stress's fork/COW phases run on real page tables.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-cow-fault.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/cow_fault.rs" -o "$tmp/cow_fault.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_cow_fault.c" \
    "$tmp/cow_fault.o" -o "$tmp/test-cow-fault"
"$tmp/test-cow-fault"
