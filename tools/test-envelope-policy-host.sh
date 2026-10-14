#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
${RUSTC:-rustc} --edition=2021 --crate-type lib --emit=obj \
    -C panic=abort -C overflow-checks=off \
    "$repo/kernel/ipc/envelope_policy.rs" -o "$tmp/envelope_policy.o"
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    "$repo/tools/tests/envelope_policy_host.c" "$tmp/envelope_policy.o" \
    -o "$tmp/envelope-policy-host"
"$tmp/envelope-policy-host"
