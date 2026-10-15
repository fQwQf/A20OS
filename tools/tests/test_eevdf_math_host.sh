#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a20-eevdf-math.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"${RUSTC:-rustc}" --edition=2021 --test "$root/kernel/proc/eevdf_math.rs" \
    -o "$tmp/eevdf-math-tests"
"$tmp/eevdf-math-tests" --nocapture
