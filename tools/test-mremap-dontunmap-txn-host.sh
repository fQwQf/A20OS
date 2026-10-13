#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-mremap-txn.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/mremap_dontunmap_txn.rs" -o "$tmp/mremap_dontunmap_txn.o"
cc -std=c11 -Wall -Wextra -Werror -I"$repo/kernel/include" \
    "$repo/tools/tests/test_mremap_dontunmap_txn.c" \
    "$repo/kernel/mm/mremap_dontunmap_txn.c" \
    "$tmp/mremap_dontunmap_txn.o" -o "$tmp/test-mremap-dontunmap-txn"
"$tmp/test-mremap-dontunmap-txn"
