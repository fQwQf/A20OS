#!/bin/sh
# Host differential harness for kernel/mm/vmar_policy.rs.  Guest coverage is
# separate: /bin/native-mm-* drives the VMAR hierarchy and ceilings through the
# Native ABI.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-vmar-policy.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/vmar_policy.rs" -o "$tmp/vmar_policy.o"
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_vmar_policy.c" \
    "$tmp/vmar_policy.o" -o "$tmp/test-vmar-policy"
"$tmp/test-vmar-policy"
