#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-svcmgr-manager.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"
python3 tools/a20idl.py user/svc/a20_services.idl \
    "$tmp_dir/a20_services_idl.c" \
    --rust-output "$tmp_dir/a20_services_idl.rs"

A20_IDL_RUST_BINDINGS="$tmp_dir/a20_services_idl.rs" \
    rustc --test --edition=2021 user/svc/svcmgr.rs \
    -o "$tmp_dir/svcmgr-manager-tests"
"$tmp_dir/svcmgr-manager-tests"
