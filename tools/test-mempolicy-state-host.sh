#!/bin/sh
# Host differential harness for kernel/mm/mempolicy_state.rs.  Guest coverage
# is separate: smoke-syscall-ext exercises the real set/get round trip.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-mempolicy.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/mempolicy_state.rs" -o "$tmp/mempolicy_state.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_mempolicy_state.c" \
    "$tmp/mempolicy_state.o" -o "$tmp/test-mempolicy-state"
"$tmp/test-mempolicy-state"
