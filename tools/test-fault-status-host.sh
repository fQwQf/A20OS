#!/bin/sh
# Host differential harness for kernel/mm/fault_status.rs.  Guest coverage is
# separate: the smoke cases assert fault_from_status non-vacuity.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-fault-status.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/fault_status.rs" -o "$tmp/fault_status.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_fault_status.c" \
    "$tmp/fault_status.o" -o "$tmp/test-fault-status"
"$tmp/test-fault-status"
