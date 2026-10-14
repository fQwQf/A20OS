#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-virtio-blk-core.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

"${RUSTC:-rustc}" --edition=2021 --test \
    "$repo_root/kernel/drivers/block/virtio_blk_core.rs" \
    -o "$tmp_dir/virtio-blk-core-tests"
"$tmp_dir/virtio-blk-core-tests"
