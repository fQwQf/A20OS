#!/bin/sh
# Host differential/failure-injection harness for kernel/mm/mmap_policy.rs.
# Compiles the production Rust module for the host and links it against the
# stub mm in tools/tests/test_mmap_policy.c.  Guest coverage is separate; this
# exercises the failure and rollback paths a guest cannot reach on demand.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-mmap-policy.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/mmap_policy.rs" -o "$tmp/mmap_policy.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_mmap_policy.c" \
    "$tmp/mmap_policy.o" -o "$tmp/test-mmap-policy"
"$tmp/test-mmap-policy"
