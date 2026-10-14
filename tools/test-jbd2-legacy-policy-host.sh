#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-jbd2-policy.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"
rustc --test --edition=2021 user/svc/jbd2_legacy_policy.rs \
    -o "$tmp_dir/jbd2-legacy-policy-tests"
"$tmp_dir/jbd2-legacy-policy-tests"
