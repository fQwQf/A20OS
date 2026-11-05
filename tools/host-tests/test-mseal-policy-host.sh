#!/bin/sh
# Host differential harness for kernel/mm/mseal_policy.rs.  Guest coverage is
# separate: mm_stress's mseal phases run the real syscall.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-mseal.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/mseal_policy.rs" -o "$tmp/mseal_policy.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_mseal_policy.c" \
    "$tmp/mseal_policy.o" -o "$tmp/test-mseal-policy"
"$tmp/test-mseal-policy"
