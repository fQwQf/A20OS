#!/bin/sh
# Host differential harness for kernel/mm/madvise_policy.rs.  Guest coverage is
# separate: mm_stress and mmprobe drive MADV_DONTNEED and the fork-policy
# advice through the real page tables.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-madvise-policy.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/madvise_policy.rs" -o "$tmp/madvise_policy.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_madvise_policy.c" \
    "$tmp/madvise_policy.o" -o "$tmp/test-madvise-policy"
"$tmp/test-madvise-policy"
