#!/bin/sh
# Host differential harness for kernel/mm/mprotect_txn.rs.  Guest coverage is
# separate: mm_stress exercises mprotect on real page tables (COW, huge-leaf
# demotion, mseal refusal, fork isolation).
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-mprotect.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/mprotect_txn.rs" -o "$tmp/mprotect_txn.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_mprotect_txn.c" \
    "$tmp/mprotect_txn.o" -o "$tmp/test-mprotect-txn"
"$tmp/test-mprotect-txn"
