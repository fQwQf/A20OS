#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
# The shared lease lives in the git common dir; in a linked worktree `.git`
# is a file, so `$root/.git/...` does not exist.
common=$(git rev-parse --path-format=absolute --git-common-dir)
out=${P3_BUILD_DIR:-$root/build/keyring-host}
mkdir -p "$out/host" "$out/tmp"
TMPDIR="$out/tmp" "$common/parallel-rewrite/run-build.sh" build-2 \
    rustc --edition=2021 --test "$root/kernel/ipc/keyring.rs" \
    -o "$out/host/keyring-tests"
"$out/host/keyring-tests"
