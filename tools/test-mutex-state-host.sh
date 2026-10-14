#!/bin/sh
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/core/mutex_state.rs -o "$tmpdir/mutex-state-tests"
"$tmpdir/mutex-state-tests"
