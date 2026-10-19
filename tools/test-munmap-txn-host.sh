#!/bin/sh
# Host differential/failure-injection harness for kernel/mm/munmap_txn.rs.
# Compiles the production Rust transaction for the host and links it against
# the mm model in tools/tests/test_munmap_txn.c.  Guest coverage is separate.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-munmap-txn.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/munmap_txn.rs" -o "$tmp/munmap_txn.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_munmap_txn.c" \
    "$tmp/munmap_txn.o" -o "$tmp/test-munmap-txn"
"$tmp/test-munmap-txn"
