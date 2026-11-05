#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-ufs-close-token.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cd "$repo_root"
rustc --edition=2021 --crate-name ufs_close_token_host --crate-type staticlib \
    tools/tests/ufs_close_token_rust_abi.rs -o "$tmp_dir/libufs-close-token-rust.a"
cc -std=gnu11 -Wall -Wextra -Werror -Ikernel/include -Iuser/svc -Iuser/liba20rt \
    tools/tests/ufs_close_token_host.c "$tmp_dir/libufs-close-token-rust.a" \
    -ldl -lpthread -lm -o "$tmp_dir/ufs-close-token-tests"
"$tmp_dir/ufs-close-token-tests"
rustc --test --edition=2021 user/svc/ufsd_protocol.rs \
    -o "$tmp_dir/ufsd-protocol-tests"
"$tmp_dir/ufsd-protocol-tests"
rustc --test --edition=2021 user/svc/ufs_lifetime.rs \
    -o "$tmp_dir/ufsd-lifetime-tests"
"$tmp_dir/ufsd-lifetime-tests"
echo "ufs close token registry host test: PASS"
