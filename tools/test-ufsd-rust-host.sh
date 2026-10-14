#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-ufsd-rust.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"
rustc --test --edition=2021 user/svc/ufsd_protocol.rs \
    -o "$tmp_dir/ufsd-protocol-tests"
"$tmp_dir/ufsd-protocol-tests"
