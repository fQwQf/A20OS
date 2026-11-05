#!/bin/sh
# Host differential harness for kernel/mm/oom_policy.rs.  Guest coverage is
# separate: smoke-oom-stress drives the real allocator pressure, victim kill
# and background reclaimer.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-oom-policy.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/oom_policy.rs" -o "$tmp/oom_policy.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_oom_policy.c" \
    "$tmp/oom_policy.o" -o "$tmp/test-oom-policy"
"$tmp/test-oom-policy"
