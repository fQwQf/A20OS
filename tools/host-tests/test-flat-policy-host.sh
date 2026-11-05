#!/bin/sh
# Host harness for the flat-space policy (kernel/mm/flat_policy.rs), with
# kernel/mm/flat_user.c providing only the bridges.
#
# During the takeover this same harness was compiled twice -- against the C
# implementation and against the Rust policy -- and passed on both, so every
# assertion below was checked to be behaviour-preserving across the migration
# (.kernel-build/evidence/fl-slice-evidence.txt).  With the C implementation
# deleted there is one leg left to run.
#
# This file has no guest runtime: CONFIG_FLAT_USER_MEMORY is a NOMMU-only
# option and this tree builds NOMMU profiles without running them, so the host
# pair is the whole verification story and both sides are built from the same
# harness source on purpose.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-flat-policy.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
    -C panic=abort -C opt-level=1 \
    "$repo/kernel/mm/flat_policy.rs" -o "$tmp/flat_policy.o"
cc -std=gnu99 -O2 -Wall -Wextra -pthread -DA20_FLAT_HOST_TEST -DCONFIG_FLAT_RUST \
   -Ikernel/include -include "$repo/tools/tests/flat_user_shim.h" \
   "$repo/tools/tests/test_flat_user.c" "$repo/kernel/mm/flat_user.c" \
   "$tmp/flat_policy.o" -o "$tmp/flat-rust"
printf 'flat policy (Rust): '
"$tmp/flat-rust"
