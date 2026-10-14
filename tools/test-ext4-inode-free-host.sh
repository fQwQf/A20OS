#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/a20-ext4-inode-free-host-$$
trap 'rm -f "$tmp"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    "$root/tools/tests/ext4_inode_free_txn_host.c" -o "$tmp"
"$tmp"
