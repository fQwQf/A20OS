#!/bin/sh
# Host differential harness for kernel/mm/wx_policy.rs and
# kernel/mm/aslr_policy.rs.  Compiles both production Rust modules for the host
# and links them against the stubs in tools/tests/test_wx_aslr_policy.c.
# Guest coverage is separate: the smoke-wx-aslr gate runs the real
# wx_aslr_test binary against the real cmdline and the real entropy source.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-wx-aslr-policy.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

for m in wx_policy aslr_policy; do
    rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
        -C panic=abort -C opt-level=1 \
        "$repo/kernel/mm/$m.rs" -o "$tmp/$m.o"
done
cc -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/test_wx_aslr_policy.c" \
    "$tmp/wx_policy.o" "$tmp/aslr_policy.o" -o "$tmp/test-wx-aslr-policy"
"$tmp/test-wx-aslr-policy"
