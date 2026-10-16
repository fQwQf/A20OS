#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
out=${P3_BUILD_DIR:-<build-dir>/p3-finish-20261016}
mkdir -p "$out/host" "$out/tmp"
TMPDIR="$out/tmp" "$root/.git/parallel-rewrite/run-build.sh" build-2 \
    rustc --edition=2021 --test "$root/kernel/ipc/keyring.rs" \
    -o "$out/host/keyring-tests"
"$out/host/keyring-tests"
